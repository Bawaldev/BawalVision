package inspection

import (
	"context"
	"testing"
	"time"
)

func TestInspectionBatchWorker_EnqueueNonBlocking(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Small buffer of size 3 to test saturation & drop
	w := &InspectionBatchWorker{
		queue:       make(chan BatchInspectionTask, 3),
		batchSize:   10,
		flushPeriod: 100 * time.Millisecond,
		ctx:         ctx,
		cancel:      cancel,
		running:     true,
	}

	task1 := BatchInspectionTask{RecordID: "task-1"}
	task2 := BatchInspectionTask{RecordID: "task-2"}
	task3 := BatchInspectionTask{RecordID: "task-3"}
	task4 := BatchInspectionTask{RecordID: "task-4"}

	if !w.Enqueue(task1) {
		t.Fatalf("expected task1 to enqueue successfully")
	}
	if !w.Enqueue(task2) {
		t.Fatalf("expected task2 to enqueue successfully")
	}
	if !w.Enqueue(task3) {
		t.Fatalf("expected task3 to enqueue successfully")
	}

	// Buffer is now full (3/3). Enqueueing task4 should drop oldest (task1) and succeed without blocking
	if !w.Enqueue(task4) {
		t.Fatalf("expected task4 to enqueue successfully after dropping oldest")
	}

	// Queue should still have exactly 3 items: task2, task3, task4
	if len(w.queue) != 3 {
		t.Fatalf("expected queue length 3, got %d", len(w.queue))
	}

	first := <-w.queue
	if first.RecordID != "task-2" {
		t.Fatalf("expected oldest task to be task-2 (task-1 dropped), got %s", first.RecordID)
	}
}

func TestEvaluateFoodQuality(t *testing.T) {
	cvConf, spoilage, temp, status := EvaluateFoodQuality("chicken", 0.90, 10.0, 24.0, "")
	if status != "PASS" {
		t.Errorf("expected PASS, got %s", status)
	}
	if cvConf != 0.90 || spoilage != 10.0 || temp != 24.0 {
		t.Errorf("unexpected evaluation values: conf=%.2f, spoilage=%.2f, temp=%.2f", cvConf, spoilage, temp)
	}

	// High spoilage score should trigger REJECT
	_, _, _, rejectStatus := EvaluateFoodQuality("fish", 0.40, 60.0, 30.0, "")
	if rejectStatus != "REJECT" {
		t.Errorf("expected REJECT, got %s", rejectStatus)
	}
}

func TestStoreTelemetryPrefersFreshWiFiOverUSB(t *testing.T) {
	store := NewInspectionStore()
	now := time.Now().Unix()

	store.StoreTelemetry(TelemetryIngressPayload{
		DeviceID:       "ESP32S3_WIFI",
		ConnectionMode: "wifi_lan",
		Timestamp:      now,
		Temperature:    37.5,
	})
	store.StoreTelemetry(TelemetryIngressPayload{
		DeviceID:       "ESP32S3_WIFI",
		ConnectionMode: "usb_serial",
		Timestamp:      now,
		Temperature:    99.0,
	})

	got, ok := store.GetLatestTelemetry("ESP32S3_WIFI")
	if !ok {
		t.Fatalf("expected telemetry to be stored")
	}
	if got.ConnectionMode != "wifi_lan" {
		t.Fatalf("expected wifi_lan to take priority, got %q", got.ConnectionMode)
	}
	if got.Temperature != 37.5 {
		t.Fatalf("expected Wi-Fi temperature to be preserved, got %.1f", got.Temperature)
	}
}

func TestStoreTelemetryBootTimestampNormalization(t *testing.T) {
	store := NewInspectionStore()
	// ESP32-S3 without SNTP sends seconds since boot (e.g. 120)
	store.StoreTelemetry(TelemetryIngressPayload{
		DeviceID:        "ESP32S3_441BF6C95678",
		FirmwareVersion: "1.0.0",
		Timestamp:       120,
		Temperature:     38.5,
		CPUTemperature:  38.5,
		CurrentCPULoad:  24,
	})

	got, ok := store.GetAnyLatestTelemetry()
	if !ok {
		t.Fatalf("expected telemetry to be stored")
	}
	nowUnix := time.Now().Unix()
	if (nowUnix - got.Timestamp) > 5 {
		t.Errorf("expected normalized server timestamp close to %d, got %d", nowUnix, got.Timestamp)
	}
}
