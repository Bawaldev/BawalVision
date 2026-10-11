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

