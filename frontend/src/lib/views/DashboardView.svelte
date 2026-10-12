<script lang="ts">
    import CameraStream from '../components/CameraStream.svelte';
    import FoodBreakdownTable from '../components/FoodBreakdownTable.svelte';
    import InferenceLog from '../components/InferenceLog.svelte';
    import StatusSummary from '../components/StatusSummary.svelte';
    import ChemicalSpoilageBar from '../components/ChemicalSpoilageBar.svelte';
    import EnvironmentalTelemetry from '../components/EnvironmentalTelemetry.svelte';
    import ActionTriggerBar from '../components/ActionTriggerBar.svelte';
    import BatchAndCloudSync from '../components/BatchAndCloudSync.svelte';

    import type { FoodDetection, TelemetryPayload, InferenceLogEntry } from '../types';

    interface Props {
        detections: FoodDetection[];
        telemetry: TelemetryPayload;
        logs: InferenceLogEntry[];
        totalAccepted: number;
        totalRejected: number;
        totalPlates: number;
        passRate: number;
        isOnline: boolean;
        hasCamera?: boolean;
        bufferedRecords: number;
        isInspecting: boolean;
        onForceReject: () => void;
        onInspectTray: () => void;
        onClearLogs: () => void;
    }

    let {
        detections = [],
        telemetry,
        logs = [],
        totalAccepted = 0,
        totalRejected = 0,
        totalPlates = 0,
        passRate = 100,
        isOnline = true,
        hasCamera = false,
        bufferedRecords = 0,
        isInspecting = false,
        onForceReject,
        onInspectTray,
        onClearLogs
    }: Props = $props();

    let avgFreshness = $derived.by(() => {
        if (!detections || detections.length === 0) return 0;
        let sum = 0;
        for (const det of detections) {
            const sp = Number(det.spoilage_score) || 0;
            sum += Math.max(0, Math.min(100, 100 - sp));
        }
        return Math.round(sum / detections.length);
    });
</script>

<div class="dashboard-main-grid">
    <!-- Left Column (Wide: Live Camera Stream, Detected Food Items Table, Inference Log) -->
    <div class="left-column">
        <!-- 1. Live Camera Stream & Detected Object Checklist -->
        <CameraStream
            {detections}
            deviceId={telemetry?.device_id}
            isLive={isOnline}
            {hasCamera}
        />

        <!-- 2. Dynamic Detected Food Items Table -->
        <FoodBreakdownTable
            {detections}
        />

        <!-- 3. Realtime Inference Log Terminal -->
        <InferenceLog
            {logs}
            onClear={onClearLogs}
        />
    </div>

    <!-- Right Column (Stacked Cards: Yield, Freshness, Telemetry, Controls, Batch Sync) -->
    <div class="right-column">
        <!-- 1. QC Inspection Yield (Accepted / Rejected Ring) -->
        <StatusSummary
            {totalAccepted}
            {totalRejected}
        />

        <!-- 2. Food Quality & Freshness (Chemical / Thermal Spoilage & Freshness Bar) -->
        <ChemicalSpoilageBar
            temperature={telemetry?.food_temperature || telemetry?.temperature}
            isSpoiled={(telemetry?.food_temperature || telemetry?.temperature || 0) > 40 || (detections && detections.some(d => d.status === 'REJECT'))}
            tSafeHours={detections && detections.length > 0 ? (telemetry?.food_temperature && telemetry.food_temperature > 20 ? 8.0 * (1.0 / (1.0 + 0.05 * (telemetry.food_temperature - 20))) : 8.0) : 0}
            hasFood={detections && detections.length > 0}
            qualityScore={avgFreshness}
        />

        <!-- 3. Thermal & Hardware Telemetry (Physical Environmental Telemetry) -->
        <EnvironmentalTelemetry
            foodTemperature={telemetry?.food_temperature || telemetry?.temperature}
            cpuTemperature={telemetry?.cpu_temperature}
            temperature={telemetry?.temperature}
            cpuLoad={telemetry?.cpu_load}
            firmwareVersion={telemetry?.firmware_version}
        />

        <!-- 4. Manual Controls (Action & Trigger Bar) -->
        <ActionTriggerBar
            {onForceReject}
            {onInspectTray}
            {isInspecting}
        />

        <!-- 5. Batch Summary & Database Sync Status -->
        <BatchAndCloudSync
            {totalPlates}
            {passRate}
            {isOnline}
            {bufferedRecords}
        />
    </div>
</div>

<style>
    .dashboard-main-grid {
        display: grid;
        grid-template-columns: minmax(0, 1fr) 360px;
        gap: 16px;
        align-items: start;
        width: 100%;
    }

    .left-column {
        display: flex;
        flex-direction: column;
        gap: 16px;
        min-width: 0;
    }

    .right-column {
        display: flex;
        flex-direction: column;
        gap: 16px;
        min-width: 0;
    }

    @media (max-width: 1200px) {
        .dashboard-main-grid {
            grid-template-columns: 1fr;
        }
    }
</style>
