package inspection

import (
	"context"
	"log"
	"strings"
	"sync"
	"time"

	"bawal_vision_backend/internal/food"
	"bawal_vision_backend/internal/model"

	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
)

// BatchInspectionTask holds pre-evaluated inspection metadata ready for bulk persistence.
type BatchInspectionTask struct {
	RecordID        string
	Request         IngressInspectionRequest
	Temperature     float64
	CPUTemp         float64
	CPULoad         uint8
	FirmwareVersion string
	IsSpoiled       bool
	TSafeHours      float64
	CreatedAt       time.Time
}

// InspectionBatchWorker decouples HTTP ingress from PostgreSQL I/O using a buffered channel
// and asynchronous batched transaction flushes.
type InspectionBatchWorker struct {
	db          *gorm.DB
	queue       chan BatchInspectionTask
	batchSize   int
	flushPeriod time.Duration
	ctx         context.Context
	cancel      context.CancelFunc
	wg          sync.WaitGroup
	running     bool
	mu          sync.Mutex
}

var GlobalBatchWorker *InspectionBatchWorker

// InitGlobalBatchWorker starts the singleton batch flush worker with configurable queue capacity and timers.
func InitGlobalBatchWorker(db *gorm.DB, bufferCap, batchSize int, flushPeriod time.Duration) *InspectionBatchWorker {
	if bufferCap <= 0 {
		bufferCap = 512
	}
	if batchSize <= 0 {
		batchSize = 25
	}
	if flushPeriod <= 0 {
		flushPeriod = 1 * time.Second
	}

	ctx, cancel := context.WithCancel(context.Background())
	w := &InspectionBatchWorker{
		db:          db,
		queue:       make(chan BatchInspectionTask, bufferCap),
		batchSize:   batchSize,
		flushPeriod: flushPeriod,
		ctx:         ctx,
		cancel:      cancel,
		running:     true,
	}

	GlobalBatchWorker = w
	w.Start()
	return w
}

// Enqueue pushes an inspection task into the buffered channel non-blockingly.
// Under extreme traffic bursts where the channel saturates, it drops the oldest item
// to prevent memory bloat and guarantee sub-millisecond HTTP response times.
func (w *InspectionBatchWorker) Enqueue(task BatchInspectionTask) bool {
	if w == nil || !w.running {
		return false
	}

	select {
	case w.queue <- task:
		return true
	default:
		// Saturated buffer: drop oldest task to maintain fresh queue and zero HTTP lock
		select {
		case <-w.queue:
		default:
		}
		select {
		case w.queue <- task:
			return true
		default:
			return false
		}
	}
}

// Start spawns the background worker goroutine for scheduled and threshold flushes.
func (w *InspectionBatchWorker) Start() {
	w.wg.Add(1)
	go func() {
		defer w.wg.Done()
		ticker := time.NewTicker(w.flushPeriod)
		defer ticker.Stop()

		batch := make([]BatchInspectionTask, 0, w.batchSize)

		for {
			select {
			case <-w.ctx.Done():
				// Drain remaining queued items on shutdown
				for {
					select {
					case task := <-w.queue:
						batch = append(batch, task)
						if len(batch) >= w.batchSize {
							w.flushBatch(batch)
							batch = make([]BatchInspectionTask, 0, w.batchSize)
						}
					default:
						if len(batch) > 0 {
							w.flushBatch(batch)
						}
						return
					}
				}

			case task := <-w.queue:
				batch = append(batch, task)
				if len(batch) >= w.batchSize {
					w.flushBatch(batch)
					batch = make([]BatchInspectionTask, 0, w.batchSize)
				}

			case <-ticker.C:
				if len(batch) > 0 {
					w.flushBatch(batch)
					batch = make([]BatchInspectionTask, 0, w.batchSize)
				}
			}
		}
	}()
}

// Stop cleanly terminates the worker and ensures any pending tasks in the queue are flushed to DB.
func (w *InspectionBatchWorker) Stop() {
	if w == nil {
		return
	}
	w.mu.Lock()
	if !w.running {
		w.mu.Unlock()
		return
	}
	w.running = false
	w.mu.Unlock()

	w.cancel()
	w.wg.Wait()
}

// flushBatch aggregates tasks into bulk SQL operations executed within a single database transaction.
func (w *InspectionBatchWorker) flushBatch(batch []BatchInspectionTask) {
	if len(batch) == 0 {
		return
	}

	start := time.Now()

	// 1. Group and deduplicate devices
	deviceMap := make(map[string]model.Device)
	for _, task := range batch {
		if task.Request.DeviceID == "" {
			continue
		}
		deviceMap[task.Request.DeviceID] = model.Device{
			ID:              task.Request.DeviceID,
			DeviceType:      "ESP32-S3",
			FirmwareVersion: task.FirmwareVersion,
			Status:          "ACTIVE",
			LastSeenAt:      &task.CreatedAt,
			UpdatedAt:       task.CreatedAt,
		}
	}
	devices := make([]model.Device, 0, len(deviceMap))
	for _, dev := range deviceMap {
		devices = append(devices, dev)
	}

	// 2. Collect unique food names across all items in batch
	uniqueNames := make(map[string]struct{})
	for _, task := range batch {
		for _, det := range task.Request.Detections {
			name := strings.TrimSpace(strings.ToLower(det.FoodName))
			if name != "" && name != "no_food" && name != "no_model" {
				uniqueNames[name] = struct{}{}
			}
		}
	}

	// 3. Execute bulk persistence in single transaction
	err := w.db.Transaction(func(tx *gorm.DB) error {
		// A. Upsert devices in batch
		if len(devices) > 0 {
			if err := tx.Clauses(clause.OnConflict{
				Columns:   []clause.Column{{Name: "id"}},
				DoUpdates: clause.AssignmentColumns([]string{"firmware_version", "last_seen_at", "updated_at", "status"}),
			}).CreateInBatches(devices, 50).Error; err != nil {
				return err
			}
		}

		// B. Resolve food items (Batch select + Batch insert for missing items)
		nameToID := make(map[string]int)
		if len(uniqueNames) > 0 {
			namesList := make([]string, 0, len(uniqueNames))
			for n := range uniqueNames {
				namesList = append(namesList, n)
			}

			var existing []food.FoodItem
			if err := tx.Where("LOWER(name) IN ?", namesList).Find(&existing).Error; err != nil {
				return err
			}
			for _, item := range existing {
				nameToID[strings.ToLower(item.Name)] = item.ID
			}

			var missing []food.FoodItem
			for _, n := range namesList {
				if _, found := nameToID[n]; !found {
					missing = append(missing, food.FoodItem{Name: n})
				}
			}
			if len(missing) > 0 {
				if err := tx.Clauses(clause.OnConflict{
					Columns:   []clause.Column{{Name: "name"}},
					DoNothing: true,
				}).CreateInBatches(missing, 50).Error; err != nil {
					return err
				}
				// Re-query to fetch newly generated IDs
				var reloaded []food.FoodItem
				if err := tx.Where("LOWER(name) IN ?", namesList).Find(&reloaded).Error; err != nil {
					return err
				}
				for _, item := range reloaded {
					nameToID[strings.ToLower(item.Name)] = item.ID
				}
			}
		}

		// C. Prepare records and inspection foods
		records := make([]InspectionRecord, 0, len(batch))
		inspectionFoods := make([]InspectionFood, 0, len(batch)*3)

		for _, task := range batch {
			lux := 120.0
			wb := "BALANCED"
			cct := 6500
			if task.Request.SceneOptical != nil {
				if task.Request.SceneOptical.LuxLuminance > 0 {
					lux = task.Request.SceneOptical.LuxLuminance
				}
				if task.Request.SceneOptical.WhiteBalanceStatus != "" {
					wb = task.Request.SceneOptical.WhiteBalanceStatus
				}
				if task.Request.SceneOptical.ColorTemperature > 0 {
					cct = task.Request.SceneOptical.ColorTemperature
				}
			}

			records = append(records, InspectionRecord{
				ID:                 task.RecordID,
				UserID:             task.Request.UserID,
				DeviceID:           task.Request.DeviceID,
				FirmwareVersion:    task.FirmwareVersion,
				CreatedAt:          task.CreatedAt,
				ImageS3URI:         task.Request.ImageS3URI,
				CPUTemperature:     task.CPUTemp,
				Temperature:        task.Temperature,
				CPULoad:            task.CPULoad,
				IsSpoiled:          task.IsSpoiled,
				TSafeHours:         task.TSafeHours,
				LuxLuminance:       lux,
				WhiteBalanceStatus: wb,
				ColorTemperature:   cct,
			})

			insertedForTask := make(map[int]bool)
			for _, det := range task.Request.Detections {
				foodName := strings.TrimSpace(strings.ToLower(det.FoodName))
				foodID, ok := nameToID[foodName]
				if !ok || insertedForTask[foodID] {
					continue
				}
				insertedForTask[foodID] = true

				itemTemp := task.Temperature
				if det.Temperature > 0 {
					itemTemp = det.Temperature
				}

				conf, spoilage, evaluatedTemp, itemStatus := EvaluateFoodQuality(
					foodName,
					det.SimilarityRate,
					det.SpoilageScore,
					itemTemp,
					det.Status,
				)

				inspectionFoods = append(inspectionFoods, InspectionFood{
					ID:             uuid.New().String(),
					InspectionID:   task.RecordID,
					FoodItemID:     foodID,
					Temperature:    evaluatedTemp,
					SimilarityRate: conf,
					SpoilageScore:  spoilage,
					Status:         itemStatus,
				})
			}
		}

		// D. Batch insert inspection records
		if len(records) > 0 {
			if err := tx.CreateInBatches(records, 50).Error; err != nil {
				return err
			}
		}

		// E. Batch insert inspection foods
		if len(inspectionFoods) > 0 {
			if err := tx.CreateInBatches(inspectionFoods, 100).Error; err != nil {
				return err
			}
		}

		return nil
	})

	if err != nil {
		log.Printf("[BATCH WORKER ERROR] Failed to flush batch of %d inspections: %v", len(batch), err)
	} else {
		log.Printf("[BATCH WORKER] Successfully flushed %d inspections to PostgreSQL in %s", len(batch), time.Since(start))
	}
}

