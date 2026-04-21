const API_BASE = document
  .querySelector('meta[name="smartplant-api"]')
  .content.replace(/\/$/, '');

const DEVICE_ID = document.querySelector('meta[name="smartplant-device"]').content;

function byId(id) {
  return document.getElementById(id);
}

function setText(id, value) {
  const el = byId(id);
  if (el) el.textContent = value;
}

const liveBadge = byId('liveBadge');
const decisionBadge = byId('decisionBadge');
const manualBadge = byId('manualBadge');

const manualDuration = byId('manualDuration');
const manualButton = byId('manualButton');
const cancelButton = byId('cancelButton');
const manualMessage = byId('manualMessage');

let soilChart;
let envChart;
let airChart;
let historyLoaded = false;

function setBadge(el, text, type = 'neutral') {
  if (!el) return;
  el.textContent = text;
  el.className = `badge badge-${type}`;
}

function fmtDate(iso) {
  if (!iso) return '--';
  return new Date(iso).toLocaleString('it-IT');
}

function fmtNum(value, digits = 1, suffix = '') {
  if (value == null || Number.isNaN(Number(value))) return `--${suffix ? ` ${suffix}` : ''}`;
  return `${Number(value).toFixed(digits)}${suffix ? ` ${suffix}` : ''}`;
}

async function request(path, options = {}) {
  const response = await fetch(`${API_BASE}${path}`, {
    headers: { 'Content-Type': 'application/json' },
    ...options
  });

  if (!response.ok) {
    const text = await response.text();
    throw new Error(text || `HTTP ${response.status}`);
  }

  return response.json();
}

function renderState(state) {
  setText('soilValue', `${Number(state.latestSoilMoisture ?? 0).toFixed(1)}%`);
  setText('soilDetail', `Soglia automatica: ${state.soilThreshold}%`);
  setText('tempValue', fmtNum(state.latestTemperature, 1, '°C'));
  setText('pressureValue', fmtNum(state.latestPressure, 1, 'hPa'));

  setText('airTempValue', fmtNum(state.latestAirTemperatureAht, 1, '°C'));
  setText('airHumidityValue', fmtNum(state.latestAirHumidity, 1, '%'));
  setText(
    'airSensorDetail',
    state.latestAhtOk ? 'AHT20 OK' : 'AHT20 non disponibile'
  );

  setText('wateringValue', state.isWatering ? 'ON' : 'OFF');
  setText(
    'wateringDetail',
    state.lastWateringAt
      ? `Ultima irrigazione: ${fmtDate(state.lastWateringAt)}`
      : 'Nessuna irrigazione registrata'
  );

  setText(
    'rainValue',
    state.rainPredicted
      ? `Sì (${Math.round((state.rainProbability || 0) * 100)}%)`
      : 'No'
  );

  setText(
    'suspendValue',
    state.suspendIrrigation
      ? `Sì${state.suspendUntil ? ` fino a ${fmtDate(state.suspendUntil)}` : ''}`
      : 'No'
  );

  setText('lastTelemetry', fmtDate(state.lastTelemetryAt));
  setText('reasonValue', state.reason || '--');
  setText('sleepValue', state.sleepTimeSec ? `${state.sleepTimeSec} sec` : '--');

  if (state.shouldWaterNow) {
    setBadge(decisionBadge, 'Acqua richiesta', 'warning');
  } else if (state.suspendIrrigation) {
    setBadge(decisionBadge, 'Sospesa per pioggia', 'danger');
  } else {
    setBadge(decisionBadge, 'Nessuna irrigazione', 'success');
  }

  if (state.manualWaterPending) {
    setBadge(manualBadge, 'Comando in coda', 'warning');
  } else {
    setBadge(manualBadge, 'Nessun comando', 'neutral');
  }
}

function buildOrUpdateChart(chartRef, canvasId, configBuilder, labels, datasets) {
  const canvas = byId(canvasId);
  if (!canvas) return chartRef;

  if (!chartRef) {
    return new Chart(canvas, configBuilder(labels, datasets));
  }

  chartRef.data.labels = labels;
  chartRef.data.datasets.forEach((dataset, index) => {
    dataset.data = datasets[index] || [];
  });
  chartRef.update();
  return chartRef;
}

function ensureCharts(history) {
  const labels = history.map((item) => fmtDate(item.timestamp));

  const soilSeries = history.map((item) => item.soilMoisture);
  const tempSeries = history.map((item) => item.temperature);
  const pressureSeries = history.map((item) => item.pressure);
  const airTempSeries = history.map((item) => item.airTemperatureAht);
  const airHumiditySeries = history.map((item) => item.airHumidity);

  soilChart = buildOrUpdateChart(
    soilChart,
    'soilChart',
    (chartLabels, chartDatasets) => ({
      type: 'line',
      data: {
        labels: chartLabels,
        datasets: [
          {
            label: 'Umidità terreno %',
            data: chartDatasets[0],
            tension: 0.25,
            fill: false
          }
        ]
      },
      options: {
        responsive: true,
        maintainAspectRatio: false
      }
    }),
    labels,
    [soilSeries]
  );

  envChart = buildOrUpdateChart(
    envChart,
    'envChart',
    (chartLabels, chartDatasets) => ({
      type: 'line',
      data: {
        labels: chartLabels,
        datasets: [
          {
            label: 'Temperatura BMP280 °C',
            data: chartDatasets[0],
            tension: 0.25,
            fill: false
          },
          {
            label: 'Pressione BMP280 hPa',
            data: chartDatasets[1],
            tension: 0.25,
            fill: false
          }
        ]
      },
      options: {
        responsive: true,
        maintainAspectRatio: false
      }
    }),
    labels,
    [tempSeries, pressureSeries]
  );

  airChart = buildOrUpdateChart(
    airChart,
    'airChart',
    (chartLabels, chartDatasets) => ({
      type: 'line',
      data: {
        labels: chartLabels,
        datasets: [
          {
            label: 'Temperatura aria AHT20 °C',
            data: chartDatasets[0],
            tension: 0.25,
            fill: false
          },
          {
            label: 'Umidità aria AHT20 %',
            data: chartDatasets[1],
            tension: 0.25,
            fill: false
          }
        ]
      },
      options: {
        responsive: true,
        maintainAspectRatio: false
      }
    }),
    labels,
    [airTempSeries, airHumiditySeries]
  );
}

async function refreshState() {
  const state = await request(`/api/state/${DEVICE_ID}`);
  renderState(state);
}

async function refreshHistory() {
  const history = await request(`/api/data/${DEVICE_ID}?limit=40`);
  ensureCharts(history);
  historyLoaded = true;
}

async function queueManualWater() {
  if (!manualButton) return;

  manualButton.disabled = true;

  try {
    const durationSec = Number((manualDuration && manualDuration.value) || 5);

    const response = await request(`/api/manual-water/${DEVICE_ID}`, {
      method: 'POST',
      body: JSON.stringify({ durationSec })
    });

    renderState(response.state);

    if (manualMessage) {
      manualMessage.textContent = response.message;
    }
  } catch (error) {
    if (manualMessage) {
      manualMessage.textContent = `Errore: ${error.message}`;
    }
  } finally {
    manualButton.disabled = false;
  }
}

async function cancelManualWater() {
  if (!cancelButton) return;

  cancelButton.disabled = true;

  try {
    const response = await request(`/api/manual-cancel/${DEVICE_ID}`, {
      method: 'POST'
    });

    renderState(response.state);

    if (manualMessage) {
      manualMessage.textContent = 'Comando manuale annullato.';
    }
  } catch (error) {
    if (manualMessage) {
      manualMessage.textContent = `Errore: ${error.message}`;
    }
  } finally {
    cancelButton.disabled = false;
  }
}

function connectSse() {
  const source = new EventSource(`${API_BASE}/api/stream/${DEVICE_ID}`);

  source.addEventListener('open', () => {
    setBadge(liveBadge, 'Live', 'success');
  });

  source.addEventListener('state', async (event) => {
    const state = JSON.parse(event.data);
    renderState(state);

    if (!historyLoaded || state.lastTelemetryAt) {
      refreshHistory().catch(console.error);
    }
  });

  source.addEventListener('ping', () => {
    setBadge(liveBadge, 'Live', 'success');
  });

  source.onerror = () => {
    setBadge(liveBadge, 'Reconnecting…', 'warning');
  };
}

if (manualButton) {
  manualButton.addEventListener('click', queueManualWater);
}

if (cancelButton) {
  cancelButton.addEventListener('click', cancelManualWater);
}

(async function init() {
  try {
    await refreshState();
    await refreshHistory();
    connectSse();

    setInterval(refreshState, 15000);
    setInterval(refreshHistory, 60000);
  } catch (error) {
    setBadge(liveBadge, 'Backend offline', 'danger');
    if (manualMessage) {
      manualMessage.textContent = `Connessione fallita: ${error.message}`;
    }
  }
})();