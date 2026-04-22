require('dotenv').config();

const express = require('express');
const cors = require('cors');
const mqtt = require('mqtt');
const mongoose = require('mongoose');
const axios = require('axios');
const PlantData = require('./models/PlantData');
const PlantState = require('./models/PlantState');

const app = express();
const PORT = Number(process.env.PORT || 3000);

const MQTT_BASE_TOPIC = process.env.MQTT_BASE_TOPIC || 'smartplant';
const DEFAULT_DEVICE_ID = process.env.DEFAULT_DEVICE_ID || 'plant-01';

const SOIL_THRESHOLD = Number(process.env.SOIL_THRESHOLD || 30);

const DEFAULT_SLEEP_SEC = Number(process.env.DEFAULT_SLEEP_SEC || 3600);
const DRY_SOIL_SLEEP_SEC = Number(
  process.env.DRY_SOIL_SLEEP_SEC || DEFAULT_SLEEP_SEC
);

const DEFAULT_AUTO_WATER_SECONDS = Number(
  process.env.DEFAULT_AUTO_WATER_SECONDS || 4
);

const DEFAULT_MANUAL_WATER_SECONDS = Number(
  process.env.DEFAULT_MANUAL_WATER_SECONDS || 5
);

const RAIN_POP_THRESHOLD = Number(process.env.RAIN_POP_THRESHOLD || 0.6);
const RAIN_MM_THRESHOLD = Number(process.env.RAIN_MM_THRESHOLD || 0.2);
const RAIN_LOOKAHEAD_HOURS = Number(
  process.env.RAIN_LOOKAHEAD_HOURS || 12
);
const WEATHER_CACHE_MINUTES = Number(
  process.env.WEATHER_CACHE_MINUTES || 30
);

app.use(express.json());

app.use(
  cors({
    origin: process.env.FRONTEND_URL ? [process.env.FRONTEND_URL] : '*'
  })
);

let mqttClient;
let mqttReady = false;
let mongoReady = false;

const sseClients = new Map();
const weatherCache = new Map();

function topicFor(deviceId, leaf) {
  return `${MQTT_BASE_TOPIC}/${deviceId}/${leaf}`;
}

function buildStatePayload(state) {
  return {
    deviceId: state.deviceId,

    soilThreshold: state.soilThreshold,
    latestSoilMoisture: state.latestSoilMoisture,
    latestRawSoil: state.latestRawSoil,

    latestTemperature: state.latestTemperature,
    latestPressure: state.latestPressure,
    latestWifiRssi: state.latestWifiRssi,

    latestAirTemperatureAht: state.latestAirTemperatureAht,
    latestAirHumidity: state.latestAirHumidity,
    latestAhtOk: state.latestAhtOk,

    lastTelemetryAt: state.lastTelemetryAt,
    lastBootAt: state.lastBootAt,

    isWatering: state.isWatering,
    lastWateringAt: state.lastWateringAt,
    lastWateringMode: state.lastWateringMode,

    manualWaterPending: state.manualWaterPending,
    commandId: state.pendingManualCommandId || '',
    manualDurationSec: state.manualDurationSec,
    autoWaterDurationSec: state.autoWaterDurationSec,

    rainPredicted: state.rainPredicted,
    rainProbability: state.rainProbability,
    suspendIrrigation: state.suspendIrrigation,
    suspendUntil: state.suspendUntil,
    weatherSummary: state.weatherSummary,

    shouldWaterNow: state.shouldWaterNow,
    decisionFromCloud: state.decisionFromCloud,
    reason: state.reason,
    sleepTimeSec: state.sleepTimeSec,
    updatedAt: state.updatedAt
  };
}

function sendSse(deviceId, eventName, payload) {
  const clients = sseClients.get(deviceId);
  if (!clients || clients.size === 0) return;

  const data = `event: ${eventName}\ndata: ${JSON.stringify(payload)}\n\n`;
  for (const res of clients) {
    res.write(data);
  }
}

async function publishState(deviceId, stateDoc) {
  const payload = buildStatePayload(stateDoc);

  if (mqttReady) {
    mqttClient.publish(topicFor(deviceId, 'state'), JSON.stringify(payload), {
      qos: 1,
      retain: true
    });
  }

  sendSse(deviceId, 'state', payload);
  return payload;
}

async function ensureState(deviceId) {
  let state = await PlantState.findOne({ deviceId });

  if (!state) {
    state = await PlantState.create({
      deviceId,
      soilThreshold: SOIL_THRESHOLD,
      manualDurationSec: DEFAULT_MANUAL_WATER_SECONDS,
      autoWaterDurationSec: DEFAULT_AUTO_WATER_SECONDS,
      sleepTimeSec: DEFAULT_SLEEP_SEC,
      reason: 'State inizializzato dal backend'
    });
  }

  return state;
}

function buildWeatherUrl() {
  const apiKey = process.env.OPENWEATHER_API_KEY;
  if (!apiKey) {
    throw new Error('OPENWEATHER_API_KEY mancante');
  }

  const lat = process.env.WEATHER_LAT;
  const lon = process.env.WEATHER_LON;

  if (lat && lon) {
    return `https://api.openweathermap.org/data/2.5/forecast?lat=${encodeURIComponent(
      lat
    )}&lon=${encodeURIComponent(
      lon
    )}&appid=${apiKey}&units=metric`;
  }

  const city = process.env.WEATHER_CITY || 'Modena,IT';
  return `https://api.openweathermap.org/data/2.5/forecast?q=${encodeURIComponent(
    city
  )}&appid=${apiKey}&units=metric`;
}

async function getWeatherDecision() {
  const cacheKey = `${process.env.WEATHER_CITY || ''}|${
    process.env.WEATHER_LAT || ''
  }|${process.env.WEATHER_LON || ''}`;

  const cached = weatherCache.get(cacheKey);
  if (cached && cached.expiresAt > Date.now()) {
    return cached.value;
  }

  try {
    const { data } = await axios.get(buildWeatherUrl(), { timeout: 8000 });

    const now = Date.now();
    const horizon = now + RAIN_LOOKAHEAD_HOURS * 3600 * 1000;

    const relevantSlots = (data.list || []).filter((slot) => {
      const slotTime = (slot.dt || 0) * 1000;
      return slotTime >= now && slotTime <= horizon;
    });

    const rainySlots = relevantSlots.filter((slot) => {
      const pop = Number(slot.pop || 0);
      const mm = Number((slot.rain && slot.rain['3h']) || 0);
      return pop >= RAIN_POP_THRESHOLD || mm >= RAIN_MM_THRESHOLD;
    });

    const maxPop = relevantSlots.reduce(
      (acc, slot) => Math.max(acc, Number(slot.pop || 0)),
      0
    );

    const firstRainy = rainySlots[0] || null;

    const value = {
      rainPredicted: rainySlots.length > 0,
      rainProbability: Number(maxPop.toFixed(2)),
      suspendIrrigation: rainySlots.length > 0,
      suspendUntil: firstRainy
        ? new Date(firstRainy.dt * 1000 + 3 * 3600 * 1000)
        : null,
      weatherSummary: firstRainy
        ? `Pioggia prevista entro ${RAIN_LOOKAHEAD_HOURS}h`
        : `Nessuna pioggia significativa nelle prossime ${RAIN_LOOKAHEAD_HOURS}h`
    };

    weatherCache.set(cacheKey, {
      value,
      expiresAt: Date.now() + WEATHER_CACHE_MINUTES * 60 * 1000
    });

    return value;
  } catch (error) {
    console.error('Errore OpenWeatherMap:', error.message);

    return {
      rainPredicted: false,
      rainProbability: 0,
      suspendIrrigation: false,
      suspendUntil: null,
      weatherSummary: 'Weather API non disponibile: fallback senza sospensione'
    };
  }
}

function computeSleepTime(soilMoisture, suspendIrrigation, manualPending) {
  if (manualPending) return 120;

  if (soilMoisture < SOIL_THRESHOLD && suspendIrrigation) {
    return DRY_SOIL_SLEEP_SEC;
  }

  if (soilMoisture < SOIL_THRESHOLD - 10) {
    return DRY_SOIL_SLEEP_SEC;
  }

  return DEFAULT_SLEEP_SEC;
}

async function republishRetainedStates() {
  const states = await PlantState.find({});
  for (const state of states) {
    await publishState(state.deviceId, state);
  }
}

async function handleTelemetry(deviceId, payload) {
  const state = await ensureState(deviceId);
  const weather = await getWeatherDecision();

  const soilMoisture = Number(payload.soilMoisture || 0);

  const automaticWaterNeeded =
    soilMoisture < state.soilThreshold && !weather.suspendIrrigation;

  const shouldWaterNow = state.manualWaterPending || automaticWaterNeeded;

  let reason = 'Terreno OK';
  if (state.manualWaterPending) {
    reason = 'Comando manuale in coda: eseguire al wake-up';
  } else if (weather.suspendIrrigation) {
    reason = 'Irrigazione sospesa per previsione di pioggia';
  } else if (automaticWaterNeeded) {
    reason = 'Terreno sotto soglia: irrigazione automatica';
  }

  state.lastTelemetryAt = new Date();

  state.latestSoilMoisture = soilMoisture;
  state.latestRawSoil = Number(payload.rawSoil || 0);

  state.latestTemperature = payload.temperature ?? null;
  state.latestPressure = payload.pressure ?? null;
  state.latestWifiRssi = payload.wifiRssi ?? null;

  state.latestAirTemperatureAht = payload.airTemperatureAht ?? null;
  state.latestAirHumidity = payload.airHumidity ?? null;
  state.latestAhtOk = Boolean(payload.ahtOk);

  state.rainPredicted = weather.rainPredicted;
  state.rainProbability = weather.rainProbability;
  state.suspendIrrigation = weather.suspendIrrigation;
  state.suspendUntil = weather.suspendUntil;
  state.weatherSummary = weather.weatherSummary;

  state.shouldWaterNow = shouldWaterNow;
  state.decisionFromCloud = true;
  state.reason = reason;
  state.sleepTimeSec = computeSleepTime(
    soilMoisture,
    weather.suspendIrrigation,
    state.manualWaterPending
  );

  await state.save();

  await PlantData.create({
    deviceId,

    soilMoisture,
    rawSoil: Number(payload.rawSoil || 0),

    temperature: payload.temperature ?? null,
    pressure: payload.pressure ?? null,
    bmpOk: Boolean(payload.bmpOk),

    airTemperatureAht: payload.airTemperatureAht ?? null,
    airHumidity: payload.airHumidity ?? null,
    ahtOk: Boolean(payload.ahtOk),

    wifiRssi: payload.wifiRssi ?? null,

    rainPredicted: weather.rainPredicted,
    rainProbability: weather.rainProbability,
    suspendIrrigation: weather.suspendIrrigation,

    isWatering: false,
    manualWaterPending: state.manualWaterPending,
    shouldWaterNow,
    decisionReason: reason
  });

  const published = await publishState(deviceId, state);
  console.log(`[telemetry] ${deviceId}`, published);
}

async function handleEvent(deviceId, payload) {
  const state = await ensureState(deviceId);
  const type = payload.type || 'unknown';

  if (type === 'boot') {
    state.lastBootAt = new Date();
    state.reason = 'ESP32 online e sincronizzato';
  }

  if (type === 'watering_started') {
    state.isWatering = true;
    state.lastWateringAt = new Date();
    state.lastWateringMode = payload.manual ? 'manual' : 'auto';
    state.reason = payload.manual
      ? 'Irrigazione manuale in corso'
      : 'Irrigazione automatica in corso';
  }

  if (type === 'watering_finished') {
    state.isWatering = false;
    state.lastWateringAt = new Date();
    state.lastWateringMode = payload.manual ? 'manual' : 'auto';

    if (payload.manual) {
      state.manualWaterPending = false;
      state.pendingManualCommandId = null;
    }

    state.shouldWaterNow = false;
    state.reason = payload.manual
      ? 'Irrigazione manuale completata'
      : 'Irrigazione automatica completata';
  }

  if (type === 'watering_skipped') {
    state.isWatering = false;
    state.reason = payload.reason || 'Irrigazione saltata';
  }

  await state.save();
  await publishState(deviceId, state);

  console.log(`[event] ${deviceId}`, type, payload.reason || '');
}

async function startMongo() {
  await mongoose.connect(process.env.MONGO_URI, {
    serverSelectionTimeoutMS: 10000
  });

  mongoReady = true;
  console.log('MongoDB Atlas connesso');
}

function startMqtt() {
  mqttClient = mqtt.connect(process.env.MQTT_URL, {
    username: process.env.MQTT_USER,
    password: process.env.MQTT_PASSWORD,
    reconnectPeriod: 5000,
    clean: true,
    connectTimeout: 10000,
    rejectUnauthorized: true,
    protocol: 'mqtts'
  });

  mqttClient.on('connect', async () => {
    mqttReady = true;
    console.log('Connesso a HiveMQ Cloud');

    mqttClient.subscribe(`${MQTT_BASE_TOPIC}/+/telemetry`, { qos: 1 });
    mqttClient.subscribe(`${MQTT_BASE_TOPIC}/+/event`, { qos: 1 });

    await republishRetainedStates();
  });

  mqttClient.on('reconnect', () => {
    console.log('Riconnessione MQTT in corso...');
  });

  mqttClient.on('error', (error) => {
    mqttReady = false;
    console.error('Errore MQTT:', error.message);
  });

  mqttClient.on('close', () => {
    mqttReady = false;
    console.warn('Connessione MQTT chiusa');
  });

  mqttClient.on('message', async (topic, message) => {
    try {
      const payload = JSON.parse(message.toString());
      const [base, deviceId, leaf] = topic.split('/');

      if (base !== MQTT_BASE_TOPIC || !deviceId) return;

      if (leaf === 'telemetry') {
        await handleTelemetry(deviceId, payload);
      }

      if (leaf === 'event') {
        await handleEvent(deviceId, payload);
      }
    } catch (error) {
      console.error(`Errore gestione topic ${topic}:`, error.message);
    }
  });
}

app.get('/api/health', (_req, res) => {
  res.json({
    ok: mongoReady && mqttReady,
    mongoReady,
    mqttReady,
    uptimeSec: Math.round(process.uptime()),
    defaultDeviceId: DEFAULT_DEVICE_ID
  });
});

app.get('/api/devices', async (_req, res) => {
  const devices = await PlantState.find(
    {},
    {
      deviceId: 1,
      latestSoilMoisture: 1,
      updatedAt: 1
    }
  ).sort({ updatedAt: -1 });

  res.json(devices);
});

app.get('/api/state/:deviceId', async (req, res) => {
  const state = await ensureState(req.params.deviceId);
  res.json(buildStatePayload(state));
});

app.get('/api/data/:deviceId', async (req, res) => {
  const limit = Math.min(Number(req.query.limit || 50), 500);

  const rows = await PlantData.find({ deviceId: req.params.deviceId })
    .sort({ timestamp: -1 })
    .limit(limit)
    .lean();

  res.json(rows.reverse());
});

app.get('/api/stream/:deviceId', async (req, res) => {
  const { deviceId } = req.params;

  res.writeHead(200, {
    'Content-Type': 'text/event-stream',
    'Cache-Control': 'no-cache, no-transform',
    Connection: 'keep-alive',
    'X-Accel-Buffering': 'no'
  });

  if (!sseClients.has(deviceId)) {
    sseClients.set(deviceId, new Set());
  }

  sseClients.get(deviceId).add(res);

  const state = await ensureState(deviceId);
  res.write(`event: state\ndata: ${JSON.stringify(buildStatePayload(state))}\n\n`);

  const heartbeat = setInterval(() => {
    res.write(`event: ping\ndata: {"ts":"${new Date().toISOString()}"}\n\n`);
  }, 25000);

  req.on('close', () => {
    clearInterval(heartbeat);

    const clients = sseClients.get(deviceId);
    if (clients) {
      clients.delete(res);
      if (clients.size === 0) {
        sseClients.delete(deviceId);
      }
    }
  });
});

app.post('/api/manual-water/:deviceId', async (req, res) => {
  const { deviceId } = req.params;

  const durationSec = Math.min(
    Math.max(Number(req.body.durationSec || DEFAULT_MANUAL_WATER_SECONDS), 1),
    30
  );

  const state = await ensureState(deviceId);
  state.manualWaterPending = true;
  state.pendingManualCommandId = new mongoose.Types.ObjectId().toString();
  state.lastManualCommandAt = new Date();
  state.manualDurationSec = durationSec;
  state.reason = 'Comando manuale accodato: attesa prossimo wake-up';

  await state.save();

  const payload = await publishState(deviceId, state);

  res.status(202).json({
    queued: true,
    message: "Comando manuale accodato. L'ESP32 lo eseguirà al prossimo risveglio.",
    state: payload
  });
});

app.post('/api/manual-cancel/:deviceId', async (req, res) => {
  const state = await ensureState(req.params.deviceId);

  state.manualWaterPending = false;
  state.pendingManualCommandId = null;
  state.reason = 'Comando manuale annullato';
  state.shouldWaterNow = false;

  await state.save();

  res.json({
    cancelled: true,
    state: await publishState(req.params.deviceId, state)
  });
});

async function bootstrap() {
  await startMongo();
  startMqtt();

  app.listen(PORT, () => {
    console.log(`SmartPlant backend in ascolto su porta ${PORT}`);
  });
}

bootstrap().catch((error) => {
  console.error('Bootstrap fallito:', error);
  process.exit(1);
});
