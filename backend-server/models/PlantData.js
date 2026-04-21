const mongoose = require('mongoose');

const PlantDataSchema = new mongoose.Schema(
  {
    deviceId: { type: String, required: true, index: true },
    soilMoisture: { type: Number, required: true },
    rawSoil: { type: Number, required: true },
    temperature: Number,
    pressure: Number,
    bmpOk: Boolean,
    wifiRssi: Number,
    rainPredicted: { type: Boolean, default: false },
    rainProbability: { type: Number, default: 0 },
    suspendIrrigation: { type: Boolean, default: false },
    isWatering: { type: Boolean, default: false },
    manualWaterPending: { type: Boolean, default: false },
    shouldWaterNow: { type: Boolean, default: false },
    decisionReason: String,
    timestamp: { type: Date, default: Date.now, index: true }
  },
  { versionKey: false }
);

PlantDataSchema.index({ deviceId: 1, timestamp: -1 });

module.exports = mongoose.model('PlantData', PlantDataSchema);
