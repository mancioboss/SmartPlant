const mongoose = require('mongoose');

const PlantStateSchema = new mongoose.Schema(
  {
    deviceId: { type: String, required: true, unique: true, index: true },
    soilThreshold: { type: Number, default: 30 },
    latestSoilMoisture: Number,
    latestRawSoil: Number,
    latestTemperature: Number,
    latestPressure: Number,
    latestWifiRssi: Number,
    lastTelemetryAt: Date,
    lastBootAt: Date,
    isWatering: { type: Boolean, default: false },
    lastWateringAt: Date,
    lastWateringMode: { type: String, enum: ['auto', 'manual', null], default: null },
    manualWaterPending: { type: Boolean, default: false },
    pendingManualCommandId: String,
    lastManualCommandAt: Date,
    manualDurationSec: { type: Number, default: 5 },
    autoWaterDurationSec: { type: Number, default: 4 },
    rainPredicted: { type: Boolean, default: false },
    rainProbability: { type: Number, default: 0 },
    suspendIrrigation: { type: Boolean, default: false },
    suspendUntil: Date,
    weatherSummary: String,
    shouldWaterNow: { type: Boolean, default: false },
    decisionFromCloud: { type: Boolean, default: true },
    reason: { type: String, default: 'Init' },
    sleepTimeSec: { type: Number, default: 3600 }
  },
  { timestamps: true, versionKey: false }
);

module.exports = mongoose.model('PlantState', PlantStateSchema);
