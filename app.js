/* ========================================
   IOT Cold Storage Mởnitor - Application Logic
   Firebase Realtime Database Integration
   ======================================== */

(function () {
    'use strict';

    // ─── Firebase Configuration ──────────────────────────
    // Điền thông tin Firebase của bạn vào đây
    const FIREBASE_CONFIG = {
        apiKey: "AIzaSyDy-6gsjTiNtdn1m715k_OcZOQvlgnvn6o",
        authDomain: "iot-sytems.firebaseapp.com",
        databaseURL: "https://iot-sytems-default-rtdb.firebaseio.com",
        projectId: "iot-sytems",
        storageBucket: "iot-sytems.firebasestorage.app",
        messagingSenderId: "798430359491",
        appId: "1:798430359491:web:f12fd5f98056909ee35776"
    };

    // ─── Firebase Database Paths ─────────────────────────
    // Cau truc du lieu trên Firebase Realtime Database:
    //
    // /sensor_data              <-- ESP32 ghi du lieu hien tai
    //   /temperature_c          float   Nhiệt độ (DHT22)
    //   /humidity               float   Độ ẩm (DHT22)
    //   /door_status            string  "OPEN" | "CLOSED"
    //   /door_open_sec          int     Thời gian cửa mở (giây)
    //   /alarm                  string  Trạng thái cảnh báo từ ESP32
    //   /sensor_id              string  Ma bo cam bien
    //   /timestamp              number  Thoi diem do (epoch ms)
    //
    // /history                  <-- ESP32 push moi 10-15 giây
    //   /{push_id}
    //     /temperature_c, humidity, door_status, door_open_sec, alarm, timestamp
    //
    // /thresholds               <-- Web ghi, ESP32 doc
    //   /temp_min, temp_max
    //   /door_delay_sec
    //
    const DB_PATHS = {
        sensorData: 'sensor_data',
        history: 'history',
        thresholds: 'thresholds',
    };

    // ─── State ───────────────────────────────────────────
    const state = {
        currentTab: 'dashboard',
        temperature: null,
        humidity: null,
        doorOpen: false,
        doorOpenSec: 0,
        doorOpenSyncTime: null,
        doorTimerInterval: null,
        espAlarm: 'NONE',
        sensorId: '---',

        historyData: [],
        alertHistory: [],
        activeAlerts: new Map(),

        thresholds: {
            temp_min: 2,
            temp_max: 8,
            door_delay_sec: 10
        },

        miniChart: null,
        historyChart: null,

        historyPage: 1,
        alertsPage: 1,
        rowsPerPage: 20,

        simInterval: null,
        trendLabels: [],
        trendTemp: [],
        trendHumi: [],

        firebaseReady: false,
        db: null,
    };

    // ─── DOM Cache ───────────────────────────────────────
    const $ = (sel) => document.querySelector(sel);
    const $$ = (sel) => document.querySelectorAll(sel);

    const dom = {
        sidebar: $('#sidebar'),
        sidebarOverlay: $('#sidebarOverlay'),
        menuToggle: $('#menuToggle'),
        navItems: $$('.nav-item'),
        tabContents: $$('.tab-content'),
        alertBadge: $('#alertBadge'),

        currentDateTime: $('#currentDateTime'),
        tempValue: $('#tempValue'),
        humiValue: $('#humiValue'),
        tempGauge: $('#tempGauge'),
        humiGauge: $('#humiGauge'),
        tempStatus: $('#tempStatus'),
        humiStatus: $('#humiStatus'),
        doorCard: $('#doorCard'),
        doorPanel: $('#doorPanel'),
        doorSeal: $('#doorSeal'),
        doorLabel: $('#doorLabel'),
        doorTimerSection: $('#doorTimerSection'),
        doorTimer: $('#doorTimer'),
        alertSummaryList: $('#alertSummaryList'),
        headerAlertStrip: $('#headerAlertStrip'),
        headerAlertText: $('#headerAlertText'),
        miniTrendCanvas: $('#miniTrendChart'),

        historyChartCanvas: $('#historyChart'),
        historyTableBody: $('#historyTableBody'),
        recordCount: $('#recordCount'),
        historyPagination: $('#historyPagination'),
        exportExcelBtn: $('#exportExcelBtn'),
        clearHistoryBtn: $('#clearHistoryBtn'),

        activeAlertsGrid: $('#activeAlertsGrid'),
        noActiveAlerts: $('#noActiveAlerts'),
        alertsTableBody: $('#alertsTableBody'),
        alertRecordCount: $('#alertRecordCount'),
        alertsPagination: $('#alertsPagination'),
        clearAlertsBtn: $('#clearAlertsBtn'),

        tempMin: $('#tempMin'),
        tempMax: $('#tempMax'),
        doorDelaySec: $('#doorDelaySec'),
        saveSettingsBtn: $('#saveSettingsBtn'),
        resetSettingsBtn: $('#resetSettingsBtn'),

        connectionStatus: $('#connectionStatus'),
        mobileStatusDot: $('#mobileStatusDot'),
        sysConnectionInfo: $('#sysConnectionInfo'),
        sysSensorId: $('#sysSensorId'),

        toastContainer: $('#toastContainer'),
    };

    // ─── Utilities ───────────────────────────────────────
    function formatTime(date) {
        return date.toLocaleTimeString('vi-VN', { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    }

    function formatDateTime(date) {
        const opts = {
            weekday: 'long', year: 'numeric', month: 'long', day: 'numeric',
            hour: '2-digit', minute: '2-digit', second: '2-digit'
        };
        return date.toLocaleDateString('vi-VN', opts);
    }

    function formatDuration(seconds) {
        const h = Math.floor(seconds / 3600);
        const m = Math.floor((seconds % 3600) / 60);
        const s = seconds % 60;
        return [h, m, s].map(v => String(v).padStart(2, '0')).join(':');
    }

    function showToast(message, type = 'info') {
        const toast = document.createElement('div');
        toast.className = `toast ${type}`;
        toast.textContent = message;
        dom.toastContainer.appendChild(toast);
        setTimeout(() => {
            toast.classList.add('fadeOut');
            setTimeout(() => toast.remove(), 300);
        }, 3000);
    }

    // ─── Navigation ──────────────────────────────────────
    function switchTab(tabName) {
        state.currentTab = tabName;
        dom.navItems.forEach(item => {
            item.classList.toggle('active', item.dataset.tab === tabName);
        });
        dom.tabContents.forEach(content => {
            content.classList.toggle('active', content.id === `tab-${tabName}`);
        });
        dom.sidebar.classList.remove('open');
        dom.sidebarOverlay.classList.remove('active');
        if (tabName === 'dashboard' && state.miniChart) {
            setTimeout(() => state.miniChart.resize(), 50);
        }
        if (tabName === 'history' && state.historyChart) {
            setTimeout(() => state.historyChart.resize(), 50);
        }
    }

    // ─── Gauges ──────────────────────────────────────────
    const GAUGE_CIRCUMFERENCE = 2 * Math.PI * 85;

    function updateGauge(gaugeEl, statusEl, value, min, max, type) {
        if (value === null) return;
        const clamped = Math.max(min, Math.min(max, value));
        const ratio = (clamped - min) / (max - min);
        const offset = GAUGE_CIRCUMFERENCE * (1 - ratio * 0.75);
        gaugeEl.style.strokeDashoffset = offset;

        let status = 'ok';
        let statusText = 'Bình thường';
        const th = state.thresholds;

        if (type === 'temp') {
            if (value < th.temp_min || value > th.temp_max) {
                status = 'danger'; statusText = 'Vượt ngưỡng';
            } else {
                status = 'ok'; statusText = 'Bình thường';
            }
        }
        // Độ ẩm: chỉ hiển thị, không cảnh báo

        statusEl.textContent = statusText;
        statusEl.className = 'gauge-status ' + status;
    }

    // ─── Door Status ─────────────────────────────────────
    function updateDoorDisplay(isOpen) {
        if (isOpen) {
            dom.doorCard.className = 'card status-card door-open';
            dom.doorPanel.classList.add('open');
            dom.doorSeal.classList.add('broken');
            dom.doorLabel.textContent = 'MO';
            dom.doorLabel.className = 'door-label open';
            dom.doorTimerSection.style.display = 'flex';
        } else {
            dom.doorCard.className = 'card status-card door-closed';
            dom.doorPanel.classList.remove('open');
            dom.doorSeal.classList.remove('broken');
            dom.doorLabel.textContent = 'DONG';
            dom.doorLabel.className = 'door-label closed';
            dom.doorTimerSection.style.display = 'none';
        }
    }

    function setDoorTimerDisplay(sec) {
        state.doorOpenSec = sec || 0;
        dom.doorTimer.textContent = formatDuration(state.doorOpenSec);
        updateAlertSummaryItems();
    }

    function resetDoorTimer() {
        if (state.doorTimerInterval) {
            clearInterval(state.doorTimerInterval);
            state.doorTimerInterval = null;
        }
        state.doorOpenSec = 0;
        state.doorOpenSyncTime = null;
        dom.doorTimer.textContent = '00:00:00';
        updateAlertSummaryItems();
    }

    // ─── Alerts ──────────────────────────────────────────
    function setAlert(type, severity, message) {
        const key = `${type}-${severity}`;
        if (state.activeAlerts.has(key)) return;

        const alert = {
            id: key,
            type: type,
            severity: severity,
            message: message,
            timestamp: new Date()
        };
        state.activeAlerts.set(key, alert);

        state.alertHistory.unshift({
            ...alert,
            resolvedAt: null
        });

        updateAlertDisplays();
    }

    function clearAlert(type) {
        const toRemove = [];
        state.activeAlerts.forEach((alert, key) => {
            if (alert.type === type) {
                toRemove.push(key);
                const histItem = state.alertHistory.find(h => h.id === key && !h.resolvedAt);
                if (histItem) histItem.resolvedAt = new Date();
            }
        });
        toRemove.forEach(key => state.activeAlerts.delete(key));
        updateAlertDisplays();
    }

    function getAlertTypeLabel(type) {
        switch (type) {
            case 'temp': return 'Nhiệt độ';
            case 'humi': return 'Độ ẩm';
            case 'door': return 'Cửa';
            case 'esp_alarm': return 'ESP32';
            default: return type;
        }
    }

    function updateAlertDisplays() {
        const count = state.activeAlerts.size;

        dom.alertBadge.textContent = count;
        dom.alertBadge.classList.toggle('visible', count > 0);

        let worstSeverity = 'ok';
        state.activeAlerts.forEach(a => {
            if (a.severity === 'danger') worstSeverity = 'danger';
            else if (a.severity === 'warning' && worstSeverity !== 'danger') worstSeverity = 'warning';
            else if (a.severity === 'lv1' && worstSeverity === 'ok') worstSeverity = 'lv1';
        });

        dom.headerAlertStrip.className = 'header-alert-strip ' + (count > 0 ? worstSeverity : 'ok');

        if (count > 0) {
            dom.headerAlertStrip.querySelector('.strip-icon').innerHTML =
                '<path fill-rule="evenodd" d="M8.257 3.099c.765-1.36 2.722-1.36 3.486 0l5.58 9.92c.75 1.334-.213 2.98-1.742 2.98H4.42c-1.53 0-2.493-1.646-1.743-2.98l5.58-9.92zM11 13a1 1 0 11-2 0 1 1 0 012 0zm-1-8a1 1 0 00-1 1v3a1 1 0 002 0V6a1 1 0 00-1-1z" clip-rule="evenodd"/>';
            dom.headerAlertText.textContent = `${count} cảnh báo đang hoạt động`;
        } else {
            dom.headerAlertStrip.querySelector('.strip-icon').innerHTML =
                '<path fill-rule="evenodd" d="M10 18a8 8 0 100-16 8 8 0 000 16zm3.707-9.293a1 1 0 00-1.414-1.414L9 10.586 7.707 9.293a1 1 0 00-1.414 1.414l2 2a1 1 0 001.414 0l4-4z" clip-rule="evenodd"/>';
            dom.headerAlertText.textContent = 'Hệ thống bình thường';
        }

        updateAlertSummaryItems();
        renderActiveAlerts();
        renderAlertsTable();
    }

    function updateAlertSummaryItems() {
        // Nhiệt độ: Chỉ báo "Bình thường" hoặc "Vượt ngưỡng"
        let tempStatus = 'ok';
        let tempText = 'Bình thường';
        if (state.temperature !== null) {
            if (state.temperature < state.thresholds.temp_min || state.temperature > state.thresholds.temp_max) {
                tempStatus = 'danger';
                tempText = 'Vượt ngưỡng';
            }
        }

        // Cửa: Chỉ báo "Đang đóng" hoặc "Đang mở" (hoặc "Mở quá lâu")
        let doorStatus = 'ok';
        let doorText = 'Đang đóng';
        if (state.doorOpen) {
            if (state.doorOpenSec >= (state.thresholds.door_delay_sec || 10)) {
                doorStatus = 'danger';
                doorText = 'Mở quá lâu';
            } else {
                doorStatus = 'warning';
                doorText = 'Đang mở';
            }
        }

        const html = `
            <div class="alert-summary-item ${tempStatus}">
                <span class="alert-dot"></span>
                <span class="alert-item-label">Nhiệt độ</span>
                <span class="alert-item-status">${tempText}</span>
            </div>
            <div class="alert-summary-item ${doorStatus}">
                <span class="alert-dot"></span>
                <span class="alert-item-label">Cửa</span>
                <span class="alert-item-status">${doorText}</span>
            </div>
        `;

        dom.alertSummaryList.innerHTML = html;
    }

    function renderActiveAlerts() {
        if (state.activeAlerts.size === 0) {
            dom.activeAlertsGrid.innerHTML = `
                <div class="empty-state">
                    <svg class="empty-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">
                        <path d="M22 11.08V12a10 10 0 1 1-5.93-9.14"/>
                        <polyline points="22 4 12 14.01 9 11.01"/>
                    </svg>
                    <span>Không có cảnh báo nào</span>
                </div>`;
            return;
        }

        let html = '';
        state.activeAlerts.forEach(alert => {
            html += `
                <div class="active-alert-card ${alert.severity}">
                    <span class="alert-card-type">${getAlertTypeLabel(alert.type)}</span>
                    <span class="alert-card-msg">${alert.message}</span>
                    <span class="alert-card-time">${formatTime(alert.timestamp)}</span>
                </div>`;
        });
        dom.activeAlertsGrid.innerHTML = html;
    }

    // ─── Check Thresholds ────────────────────────────────
    function checkThresholds(temp, humi) {
        const th = state.thresholds;
        if (temp < th.temp_min || temp > th.temp_max) {
            setAlert('temp', 'danger', 'Nhiệt độ vượt ngưỡng');
        } else {
            clearAlert('temp');
        }
    }

    // ─── Process ESP32 Alarm Field ───────────────────────
    function processEspAlarm(alarm) {
        if (!alarm || alarm === 'NONE' || alarm === '0' || alarm === '') {
            if (state.espAlarm !== 'NONE') {
                clearAlert('esp_alarm');
            }
            state.espAlarm = 'NONE';
            return;
        }

        state.espAlarm = alarm;

        if (alarm === '1') {
            setAlert('esp_alarm', 'warning', 'ESP32: Đang mở cửa');
        } else if (alarm === '2') {
            setAlert('esp_alarm', 'danger', 'ESP32: Cần xử lý (Đèn đỏ + Còi)');
        } else {
            const alarmMap = {
                'TEMP_HIGH': { severity: 'danger', msg: 'ESP32: Nhiệt độ vượt ngưỡng cao' },
                'TEMP_LOW': { severity: 'danger', msg: 'ESP32: Nhiệt độ dưới ngưỡng thấp' },
                'DOOR_OPEN_LONG': { severity: 'danger', msg: 'ESP32: Cửa mở quá lâu' },
            };
            const mapped = alarmMap[alarm];
            if (mapped) {
                setAlert('esp_alarm', mapped.severity, mapped.msg);
            } else {
                setAlert('esp_alarm', 'warning', 'ESP32: ' + alarm);
            }
        }
    }

    function updateSensorData(data) {
        const now = new Date();
        const temp = data.temperature_c;
        const humi = data.humidity;
        const doorStatus = data.door_status;
        const doorOpenSec = data.door_open_sec || 0;
        const alarm = data.alarm || 'NONE';
        const sensorId = data.sensor_id || '---';
        const isOpen = (doorStatus === 'OPEN');
        const effectiveDoorSec = isOpen ? (parseInt(doorOpenSec, 10) || 0) : 0;

        // Cập nhật giá trị hiển thị Nhiệt độ & Đồng hồ đo (Gauges)
        if (temp !== undefined && temp !== null && !isNaN(temp)) {
            state.temperature = temp;
            if (dom.tempValue) dom.tempValue.textContent = temp.toFixed(1);
            if (dom.tempGauge && dom.tempStatus) updateGauge(dom.tempGauge, dom.tempStatus, temp, -10, 40, 'temp');
        }

        // Cập nhật giá trị hiển thị Độ ẩm & Đồng hồ đo (Gauges)
        if (humi !== undefined && humi !== null && !isNaN(humi)) {
            state.humidity = humi;
            if (dom.humiValue) dom.humiValue.textContent = humi.toFixed(0);
            if (dom.humiGauge && dom.humiStatus) updateGauge(dom.humiGauge, dom.humiStatus, humi, 0, 100, 'humi');
        }

        if (sensorId && dom.sysSensorId) {
            state.sensorId = sensorId;
            dom.sysSensorId.textContent = sensorId;
        }

        if (isOpen !== state.doorOpen) {
            state.doorOpen = isOpen;
            updateDoorDisplay(isOpen);
        }

        if (isOpen) {
            // Hiển thị trực tiếp số giây nhận từ ESP32 qua Firebase
            setDoorTimerDisplay(effectiveDoorSec);

            const delayThreshold = state.thresholds.door_delay_sec || 10;
            if (effectiveDoorSec >= delayThreshold || alarm === 'DOOR_OPEN_LONG' || alarm === '2') {
                clearAlert('door');
                setAlert('door', 'danger', 'Cửa mở quá lâu');
            } else {
                clearAlert('door');
                setAlert('door', 'warning', 'Cửa đang mở');
            }
        } else {
            // Cửa đóng -> ngay lập tức reset về 00:00:00 và xóa cảnh báo cửa
            clearAlert('door');
            resetDoorTimer();
        }

        processEspAlarm(alarm);
        checkThresholds(temp, humi);
        updateAlertSummaryItems();

        // Xu hướng thời gian thực trên Dashboard (chỉ hiển thị xem, không ghi lịch sử)
        const timeStr = formatTime(data.timestamp ? new Date(data.timestamp) : now);
        state.trendLabels.push(timeStr);
        state.trendTemp.push(temp);
        state.trendHumi.push(humi);

        if (state.trendLabels.length > 60) {
            state.trendLabels.shift();
            state.trendTemp.shift();
            state.trendHumi.shift();
        }

        updateMiniChart();
    }

    // ─── Charts ──────────────────────────────────────────
    const chartBaseConfig = {
        responsive: true,
        maintainAspectRatio: false,
        interaction: {
            mode: 'index',
            intersect: false,
        },
        plugins: {
            legend: { display: false },
            tooltip: {
                backgroundColor: '#ffffff',
                titleColor: '#0f172a',
                bodyColor: '#475569',
                borderColor: '#e2e8f0',
                borderWidth: 1,
                cornerRadius: 8,
                padding: 10,
                titleFont: { family: 'Inter', weight: '600' },
                bodyFont: { family: 'Inter' },
            }
        },
        scales: {
            x: {
                grid: { color: 'rgba(203,213,225,0.4)', drawBorder: false },
                ticks: { color: '#64748b', font: { family: 'Inter', size: 10 }, maxTicksLimit: 10 },
                border: { display: false }
            },
            y: {
                grid: { color: 'rgba(203,213,225,0.4)', drawBorder: false },
                ticks: { color: '#64748b', font: { family: 'Inter', size: 10 } },
                border: { display: false }
            }
        }
    };

    function initMiniChart() {
        const ctx = dom.miniTrendCanvas.getContext('2d');
        state.miniChart = new Chart(ctx, {
            type: 'line',
            data: {
                labels: state.trendLabels,
                datasets: [
                    {
                        label: 'Nhiệt độ',
                        data: state.trendTemp,
                        borderColor: '#3b82f6',
                        backgroundColor: 'rgba(59,130,246,0.08)',
                        borderWidth: 2,
                        pointRadius: 0,
                        pointHoverRadius: 4,
                        tension: 0.35,
                        fill: true,
                    },
                    {
                        label: 'Độ ẩm',
                        data: state.trendHumi,
                        borderColor: '#06b6d4',
                        backgroundColor: 'rgba(6,182,212,0.06)',
                        borderWidth: 2,
                        pointRadius: 0,
                        pointHoverRadius: 4,
                        tension: 0.35,
                        fill: true,
                        yAxisID: 'y1',
                    }
                ]
            },
            options: {
                ...chartBaseConfig,
                scales: {
                    ...chartBaseConfig.scales,
                    y: {
                        ...chartBaseConfig.scales.y,
                        position: 'left',
                        title: { display: true, text: 'C', color: '#3b82f6', font: { family: 'Inter', size: 11 } }
                    },
                    y1: {
                        ...chartBaseConfig.scales.y,
                        position: 'right',
                        title: { display: true, text: '%', color: '#06b6d4', font: { family: 'Inter', size: 11 } },
                        grid: { drawOnChartArea: false }
                    }
                }
            }
        });
    }

    function updateMiniChart() {
        if (!state.miniChart) return;
        state.miniChart.data.labels = state.trendLabels;
        state.miniChart.data.datasets[0].data = state.trendTemp;
        state.miniChart.data.datasets[1].data = state.trendHumi;
        state.miniChart.update('none');
    }

    function initHistoryChart() {
        const ctx = dom.historyChartCanvas.getContext('2d');
        state.historyChart = new Chart(ctx, {
            type: 'line',
            data: {
                labels: [],
                datasets: [
                    {
                        label: 'Nhiệt độ (C)',
                        data: [],
                        borderColor: '#3b82f6',
                        backgroundColor: 'rgba(59,130,246,0.1)',
                        borderWidth: 2,
                        pointRadius: 1.5,
                        pointHoverRadius: 5,
                        pointBackgroundColor: '#3b82f6',
                        tension: 0.3,
                        fill: true,
                    },
                    {
                        label: 'Độ ẩm (%)',
                        data: [],
                        borderColor: '#06b6d4',
                        backgroundColor: 'rgba(6,182,212,0.08)',
                        borderWidth: 2,
                        pointRadius: 1.5,
                        pointHoverRadius: 5,
                        pointBackgroundColor: '#06b6d4',
                        tension: 0.3,
                        fill: true,
                        yAxisID: 'y1',
                    }
                ]
            },
            options: {
                ...chartBaseConfig,
                plugins: {
                    ...chartBaseConfig.plugins,
                    legend: {
                        display: true,
                        labels: {
                            color: '#475569', font: { family: 'Inter', size: 12 },
                            usePointStyle: true,
                            pointStyle: 'circle',
                            padding: 20,
                        }
                    }
                },
                scales: {
                    ...chartBaseConfig.scales,
                    x: {
                        ...chartBaseConfig.scales.x,
                        ticks: { ...chartBaseConfig.scales.x.ticks, maxTicksLimit: 15 }
                    },
                    y: {
                        ...chartBaseConfig.scales.y,
                        position: 'left',
                        title: { display: true, text: 'Nhiệt độ (C)', color: '#3b82f6', font: { family: 'Inter', size: 12, weight: '500' } }
                    },
                    y1: {
                        ...chartBaseConfig.scales.y,
                        position: 'right',
                        title: { display: true, text: 'Độ ẩm (%)', color: '#06b6d4', font: { family: 'Inter', size: 12, weight: '500' } },
                        grid: { drawOnChartArea: false }
                    }
                }
            }
        });
    }

    function updateHistoryChart() {
        if (!state.historyChart) return;
        const data = state.historyData.slice(0, 200).reverse();
        state.historyChart.data.labels = data.map(d => formatTime(d.timestamp));
        state.historyChart.data.datasets[0].data = data.map(d => d.temperature_c);
        state.historyChart.data.datasets[1].data = data.map(d => d.humidity);
        state.historyChart.update('none');
    }

    // ─── Tables ──────────────────────────────────────────
    function renderHistoryTable() {
        const total = state.historyData.length;
        const totalPages = Math.max(1, Math.ceil(total / state.rowsPerPage));
        state.historyPage = Math.min(state.historyPage, totalPages);
        const start = (state.historyPage - 1) * state.rowsPerPage;
        const end = Math.min(start + state.rowsPerPage, total);
        const pageData = state.historyData.slice(start, end);

        dom.recordCount.textContent = `${total} ban ghi`;

        let html = '';
        pageData.forEach((row, i) => {
            const idx = start + i + 1;
            const doorClass = row.door_status === 'OPEN' ? 'td-door-open' : 'td-door-closed';
            const doorText = row.door_status === 'OPEN' ? 'Mở' : 'Đóng';
            html += `
                <tr>
                    <td>${idx}</td>
                    <td>${formatTime(row.timestamp)}</td>
                    <td class="td-temp">${row.temperature_c.toFixed(1)}</td>
                    <td class="td-humi">${row.humidity.toFixed(1)}</td>
                    <td class="${doorClass}">${doorText}</td>
                </tr>`;
        });
        dom.historyTableBody.innerHTML = html || '<tr><td colspan="5" style="text-align:center;padding:24px;color:var(--text-muted)">Chua co du lieu</td></tr>';

        renderPagination(dom.historyPagination, state.historyPage, totalPages, (p) => {
            state.historyPage = p;
            renderHistoryTable();
        });
    }

    function renderAlertsTable() {
        const total = state.alertHistory.length;
        const totalPages = Math.max(1, Math.ceil(total / state.rowsPerPage));
        state.alertsPage = Math.min(state.alertsPage, totalPages);
        const start = (state.alertsPage - 1) * state.rowsPerPage;
        const end = Math.min(start + state.rowsPerPage, total);
        const pageData = state.alertHistory.slice(start, end);

        dom.alertRecordCount.textContent = `${total} ban ghi`;

        let html = '';
        pageData.forEach((row, i) => {
            const idx = start + i + 1;
            const severityClass = row.severity;
            const severityLabel = row.severity === 'danger' ? 'Cần xử lý' : row.severity === 'warning' ? 'Đang mở cửa' : 'Bình thường';
            html += `
                <tr>
                    <td>${idx}</td>
                    <td>${formatTime(row.timestamp)}</td>
                    <td>${getAlertTypeLabel(row.type)}</td>
                    <td><span class="severity-badge ${severityClass}">${severityLabel}</span></td>
                    <td>${row.message}</td>
                </tr>`;
        });
        dom.alertsTableBody.innerHTML = html || '<tr><td colspan="5" style="text-align:center;padding:24px;color:var(--text-muted)">Chưa có cảnh báo</td></tr>';

        renderPagination(dom.alertsPagination, state.alertsPage, totalPages, (p) => {
            state.alertsPage = p;
            renderAlertsTable();
        });
    }

    function renderPagination(container, currentPage, totalPages, callback) {
        if (totalPages <= 1) {
            container.innerHTML = '';
            return;
        }
        let html = '';
        html += `<button class="page-btn" ${currentPage === 1 ? 'disabled' : ''} data-page="${currentPage - 1}">&lt;</button>`;
        const range = [];
        for (let i = 1; i <= totalPages; i++) {
            if (i === 1 || i === totalPages || (i >= currentPage - 2 && i <= currentPage + 2)) {
                range.push(i);
            } else if (range[range.length - 1] !== '...') {
                range.push('...');
            }
        }
        range.forEach(p => {
            if (p === '...') {
                html += `<span style="color:var(--text-muted);padding:0 4px;">...</span>`;
            } else {
                html += `<button class="page-btn ${p === currentPage ? 'active' : ''}" data-page="${p}">${p}</button>`;
            }
        });
        html += `<button class="page-btn" ${currentPage === totalPages ? 'disabled' : ''} data-page="${currentPage + 1}">&gt;</button>`;
        container.innerHTML = html;
        container.querySelectorAll('.page-btn:not(:disabled)').forEach(btn => {
            btn.addEventListener('click', () => callback(parseInt(btn.dataset.page)));
        });
    }

    // ─── Excel Export ─────────────────────────────────────
    function downloadExcelBlob(buffer, filename) {
        const blob = new Blob([buffer], { type: 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet' });
        if (typeof saveAs === 'function') {
            saveAs(blob, filename);
            return;
        }
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = filename;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
    }

    function formatExcelDate(ts) {
        if (!ts) return new Date().toLocaleString('vi-VN');
        const d = ts instanceof Date ? ts : new Date(ts);
        return isNaN(d.getTime()) ? String(ts) : d.toLocaleString('vi-VN');
    }

    function formatExcelNum(val) {
        const n = parseFloat(val);
        return isNaN(n) ? '0.0' : n.toFixed(1);
    }

    async function exportToExcel() {
        // Collect rows: prefer historyData, fallback to current realtime reading if history is empty
        let rowsToExport = [];
        if (state.historyData && state.historyData.length > 0) {
            rowsToExport = state.historyData;
        } else if (state.temperature !== null) {
            rowsToExport = [{
                timestamp: new Date(),
                temperature_c: state.temperature,
                humidity: state.humidity !== null ? state.humidity : 0,
                door_status: state.doorOpen ? 'OPEN' : 'CLOSED',
                door_open_sec: state.doorOpenSec || 0,
                alarm: state.espAlarm || 'NONE'
            }];
        } else {
            showToast('Không có dữ liệu để xuất', 'error');
            return;
        }

        try {
            if (typeof ExcelJS !== 'undefined') {
                const workbook = new ExcelJS.Workbook();
                const sheet = workbook.addWorksheet('Lịch sử đo lường');

                sheet.columns = [
                    { header: 'STT', key: 'stt', width: 8 },
                    { header: 'Thời gian', key: 'time', width: 22 },
                    { header: 'Nhiệt độ (°C)', key: 'temp', width: 16 },
                    { header: 'Độ ẩm (%)', key: 'humi', width: 16 },
                    { header: 'Trạng thái cửa', key: 'door', width: 18 },
                    { header: 'Thời gian mở (s)', key: 'door_sec', width: 18 },
                    { header: 'Alarm', key: 'alarm', width: 20 }
                ];

                // Style header row
                const headerRow = sheet.getRow(1);
                headerRow.font = { bold: true, color: { argb: 'FF1E293B' } };
                headerRow.fill = {
                    type: 'pattern',
                    pattern: 'solid',
                    fgColor: { argb: 'FFF1F5F9' }
                };

                rowsToExport.forEach((row, i) => {
                    const isDoorOpen = (row.door_status === 'OPEN');
                    const doorText = isDoorOpen ? 'OPEN' : 'CLOSED';
                    // Cửa đã ĐÓNG thì thời gian mở bắt buộc là 0 giây
                    const doorSec = isDoorOpen ? (parseInt(row.door_open_sec) || 0) : 0;

                    let alarmStr = 'Bình thường';
                    const a = String(row.alarm || '0').toUpperCase();
                    const isTempAlert = a.includes('TEMP') || a.includes('HIGH') || a.includes('LOW');
                    const isDoorLong = a.includes('LONG') || (isDoorOpen && doorSec >= (state.thresholds.door_delay_sec || 10));

                    if (a === '2' || isTempAlert || isDoorLong) {
                        alarmStr = 'Cần xử lý';
                    } else if (isDoorOpen) {
                        alarmStr = 'Đang mở cửa';
                    } else {
                        alarmStr = 'Bình thường';
                    }

                    const newRow = sheet.addRow({
                        stt: i + 1,
                        time: formatExcelDate(row.timestamp),
                        temp: formatExcelNum(row.temperature_c),
                        humi: formatExcelNum(row.humidity),
                        door: doorText,
                        door_sec: doorSec,
                        alarm: alarmStr
                    });

                    // Set red color for door status (OPEN and CLOSED)
                    const doorCell = newRow.getCell(5);
                    doorCell.font = { color: { argb: 'FFFF0000' }, bold: true };

                    // Set red color for Alarm status
                    const alarmCell = newRow.getCell(7);
                    alarmCell.font = { color: { argb: 'FFFF0000' }, bold: true };
                });

                const buffer = await workbook.xlsx.writeBuffer();
                const fileName = 'LichSu_KhoLanh_' + new Date().toISOString().slice(0, 10) + '.xlsx';
                downloadExcelBlob(buffer, fileName);
                showToast('Đã xuất file Excel (' + rowsToExport.length + ' bản ghi)', 'success');
            } else {
                showToast('Chưa tải xong thư viện Excel, vui lòng thử lại', 'error');
            }
        } catch (err) {
            console.error('Export error:', err);
            showToast('Lỗi khi xuất file Excel: ' + (err.message || err), 'error');
        }
    }

    function loadThresholds() {
        const saved = localStorage.getItem('coldvault_thresholds');
        if (saved) {
            try {
                Object.assign(state.thresholds, JSON.parse(saved));
            } catch (e) { /* ignore */ }
        }
        if (dom.tempMin) dom.tempMin.value = state.thresholds.temp_min;
        if (dom.tempMax) dom.tempMax.value = state.thresholds.temp_max;
        if (dom.doorDelaySec) dom.doorDelaySec.value = state.thresholds.door_delay_sec;

        const keys = ['tempMin', 'tempMax', 'doorDelaySec'];
        keys.forEach(id => {
            const slider = document.getElementById('slider_' + id);
            const input = dom[id];
            if (slider && input) {
                slider.value = input.value;
            }
        });
    }

    
    
    let autoSaveTimeout = null;
    function triggerAutoSave() {
        if (autoSaveTimeout) clearTimeout(autoSaveTimeout);
        autoSaveTimeout = setTimeout(() => {
            saveThresholds();
        }, 300);
    }

    function setupThresholdInputs() {
        ['tempMin', 'tempMax', 'doorDelaySec'].forEach(id => {
            const input = dom[id];
            const slider = document.getElementById('slider_' + id);
            if (input && slider) {
                slider.addEventListener('input', () => {
                    input.value = slider.value;
                    triggerAutoSave();
                });
                input.addEventListener('input', () => {
                    slider.value = input.value;
                    triggerAutoSave();
                });
                input.addEventListener('change', () => {
                    slider.value = input.value;
                    saveThresholds();
                });
            }
        });
    }

    function saveThresholds() {
        state.thresholds.temp_min = parseFloat(dom.tempMin.value) || 2;
        state.thresholds.temp_max = parseFloat(dom.tempMax.value) || 8;
        state.thresholds.door_delay_sec = parseInt(dom.doorDelaySec.value) || 10;

        localStorage.setItem('coldvault_thresholds', JSON.stringify(state.thresholds));

        if (state.firebaseReady && state.db) {
            state.db.ref(DB_PATHS.thresholds).set(state.thresholds)
                .then(() => showToast('Đã lưu và đồng bộ lên Firebase', 'success'))
                .catch((err) => {
                    console.error('Firebase save error:', err);
                    showToast('Đã lưu local, lỗi đồng bộ Firebase', 'error');
                });
        } else {
            showToast('Đã lưu cài đặt (chưa kết nối Firebase)', 'success');
        }

        if (state.temperature !== null) {
            checkThresholds(state.temperature, state.humidity);
        }
    }

    function resetThresholds() {
        state.thresholds = {
            temp_min: 2,
            temp_max: 8,
            door_delay_sec: 10
        };
        localStorage.removeItem('coldvault_thresholds');
        loadThresholds();
        setupThresholdInputs();
        showToast('Đã đặt lại mặc định', 'info');
    }

    function setConnectionStatus(status) {
        const dotClass = status === 'online' ? 'online' : status === 'warning' ? 'warning' : 'offline';
        const text = status === 'online' ? 'Đã kết nối' : status === 'warning' ? 'Mất kết nối' : 'Chờ kết nối';
        const statusDot = dom.connectionStatus.querySelector('.status-dot');
        const statusText = dom.connectionStatus.querySelector('.status-text');
        statusDot.className = 'status-dot ' + dotClass;
        statusText.textContent = text;
        dom.mobileStatusDot.className = 'status-dot ' + dotClass;
        dom.sysConnectionInfo.textContent = text;
    }

    // ─── Clock ───────────────────────────────────────────
    function updateClock() {
        dom.currentDateTime.textContent = formatDateTime(new Date());
    }

    // ─── Firebase Integration ────────────────────────────
    function initFirebase() {
        if (!FIREBASE_CONFIG.apiKey || !FIREBASE_CONFIG.databaseURL) {
            console.log('Firebase chưa cấu hình. Chạy chế độ mô phỏng.');
            setConnectionStatus('offline');
            startSimulation();
            return;
        }

        try {
            firebase.initializeApp(FIREBASE_CONFIG);
            state.db = firebase.database();
            state.firebaseReady = true;
            console.log('Firebase da khoi tao thanh cong');
            setupFirebaseListeners();
        } catch (err) {
            console.error('Loi khoi tao Firebase:', err);
            showToast('Lỗi kết nối Firebase, chạy mô phỏng', 'error');
            startSimulation();
        }
    }

    function setupFirebaseListeners() {
        const db = state.db;

        // Theo doi trang thai ket noi
        db.ref('.info/connected').on('value', (snapshot) => {
            if (snapshot.val() === true) {
                setConnectionStatus('online');
            } else {
                setConnectionStatus('warning');
            }
        });

        // Nhan du lieu cam bien real-time
        db.ref(DB_PATHS.sensorData).on('value', (snapshot) => {
            const data = snapshot.val();
            if (data && data.temperature_c !== undefined) {
                updateSensorData({
                    temperature_c: parseFloat(data.temperature_c) || 0,
                    humidity: parseFloat(data.humidity) || 0,
                    door_status: data.door_status || 'CLOSED',
                    door_open_sec: parseInt(data.door_open_sec) || 0,
                    alarm: data.alarm || 'NONE',
                    sensor_id: data.sensor_id || '---',
                    timestamp: data.timestamp || Date.now()
                });
            }
        });

        // Đọc ngưỡng cảnh báo từ Firebase (lắng nghe thời gian thực)
        db.ref(DB_PATHS.thresholds).on('value', (snapshot) => {
            const data = snapshot.val();
            if (data) {
                Object.assign(state.thresholds, data);
                loadThresholds();
                console.log('Đã cập nhật ngưỡng cảnh báo từ Firebase:', data);
            } else {
                // Ghi nguong mac dinh len Firebase lan dau
                db.ref(DB_PATHS.thresholds).set(state.thresholds);
            }
        });

        // Doc lich su tu Firebase (gioi han 200 ban ghi gan nhat)
        db.ref(DB_PATHS.history).orderByChild('timestamp').limitToLast(200).on('child_added', (snapshot) => {
            const data = snapshot.val();
            if (data && data.temperature_c !== undefined) {
                const record = {
                    timestamp: new Date(data.timestamp || Date.now()),
                    temperature_c: parseFloat(data.temperature_c) || 0,
                    humidity: parseFloat(data.humidity) || 0,
                    door_status: data.door_status || 'CLOSED',
                    door_open_sec: parseInt(data.door_open_sec) || 0,
                    alarm: data.alarm || 'NONE'
                };

                // Tranh trung lap voi du lieu tu sensor_data listener
                const isDuplicate = state.historyData.some(h =>
                    Math.abs(h.timestamp.getTime() - record.timestamp.getTime()) < 2000 &&
                    h.temperature_c === record.temperature_c
                );

                if (!isDuplicate) {
                    state.historyData.push(record);
                    state.historyData.sort((a, b) => b.timestamp - a.timestamp);
                    if (state.historyData.length > 2000) {
                        state.historyData = state.historyData.slice(0, 2000);
                    }
                    renderHistoryTable();
                    updateHistoryChart();
                }
            }
        });
    }

    // ─── Simulation (khi chua co Firebase) ───────────────
    let simTemp = 5.0;
    let simHumi = 65;
    let simDoor = false;
    let simDoorCounter = 0;
    let simDoorOpenSec = 0;

    function simulateSensorData() {
        simTemp += (Math.random() - 0.48) * 0.6;
        simTemp = Math.max(-8, Math.min(35, simTemp));

        simHumi += (Math.random() - 0.5) * 1.2;
        simHumi = Math.max(15, Math.min(95, simHumi));

        simDoorCounter++;
        if (simDoorCounter > 15 + Math.floor(Math.random() * 25)) {
            simDoor = !simDoor;
            simDoorCounter = 0;
            simDoorOpenSec = 0;
        }

        if (simDoor) {
            simDoorOpenSec += 5;
        }

        let alarm = 'NONE';
        if (simTemp > state.thresholds.temp_max) alarm = 'TEMP_HIGH';
        else if (simTemp < state.thresholds.temp_min) alarm = 'TEMP_LOW';
        else if (simDoor && simDoorOpenSec >= state.thresholds.door_delay_sec) alarm = 'DOOR_OPEN_LONG';
        else if (simDoor) alarm = '1';

        updateSensorData({
            temperature_c: Math.round(simTemp * 10) / 10,
            humidity: Math.round(simHumi * 10) / 10,
            door_status: simDoor ? 'OPEN' : 'CLOSED',
            door_open_sec: simDoorOpenSec,
            alarm: alarm,
            sensor_id: 'SIM_ESP32',
            timestamp: Date.now()
        });
    }

    function startSimulation() {
        setConnectionStatus('online');
        simulateSensorData();
        state.simInterval = setInterval(() => {
            simulateSensorData();
            // Ghi lịch sử mô phỏng đúng 5s một lần khi không có Firebase
            if (!state.firebaseReady && state.temperature !== null) {
                state.historyData.unshift({
                    timestamp: new Date(),
                    temperature_c: state.temperature,
                    humidity: state.humidity,
                    door_status: state.doorOpen ? 'OPEN' : 'CLOSED',
                    door_open_sec: state.doorOpen ? state.doorOpenSec : 0,
                    alarm: state.espAlarm || 'NONE'
                });
                if (state.historyData.length > 2000) state.historyData.pop();
                renderHistoryTable();
                updateHistoryChart();
            }
        }, 5000);
        showToast('Chế độ mô phỏng (chưa kết nối Firebase)', 'info');
    }

    // ─── Event Bindings ──────────────────────────────────
    function bindEvents() {
        dom.navItems.forEach(item => {
            item.addEventListener('click', (e) => {
                e.preventDefault();
                switchTab(item.dataset.tab);
            });
        });

        dom.menuToggle.addEventListener('click', () => {
            dom.sidebar.classList.toggle('open');
            dom.sidebarOverlay.classList.toggle('active');
        });
        dom.sidebarOverlay.addEventListener('click', () => {
            dom.sidebar.classList.remove('open');
            dom.sidebarOverlay.classList.remove('active');
        });

        dom.exportExcelBtn.addEventListener('click', exportToExcel);

        dom.clearHistoryBtn.addEventListener('click', () => {
            state.historyData = [];
            state.historyPage = 1;
            renderHistoryTable();
            updateHistoryChart();
            showToast('Da xoa lich su do luong', 'info');
        });
        dom.clearAlertsBtn.addEventListener('click', () => {
            state.alertHistory = [];
            state.alertsPage = 1;
            renderAlertsTable();
            showToast('Đã xóa lịch sử cảnh báo', 'info');
        });

        dom.saveSettingsBtn.addEventListener('click', saveThresholds);
        ['tempMin', 'tempMax', 'doorDelaySec'].forEach(id => {
            const input = dom[id];
            const slider = document.getElementById('slider_' + id);
            if (input) {
                input.addEventListener('input', triggerAutoSave);
                input.addEventListener('change', triggerAutoSave);
            }
            if (slider) {
                slider.addEventListener('input', triggerAutoSave);
                slider.addEventListener('change', triggerAutoSave);
            }
        });
        dom.resetSettingsBtn.addEventListener('click', resetThresholds);

        document.addEventListener('keydown', (e) => {
            if (e.target.tagName === 'INPUT') return;
            const tabs = ['dashboard', 'history', 'alerts', 'control'];
            if (e.key >= '1' && e.key <= '4') {
                switchTab(tabs[parseInt(e.key) - 1]);
            }
        });
    }

    // ─── Init ────────────────────────────────────────────
    function init() {
        bindEvents();
        loadThresholds();
        initMiniChart();
        initHistoryChart();
        renderHistoryTable();
        renderAlertsTable();
        updateAlertDisplays();
        updateClock();
        setInterval(updateClock, 1000);

        dom.tempGauge.style.strokeDasharray = GAUGE_CIRCUMFERENCE;
        dom.tempGauge.style.strokeDashoffset = GAUGE_CIRCUMFERENCE;
        dom.humiGauge.style.strokeDasharray = GAUGE_CIRCUMFERENCE;
        dom.humiGauge.style.strokeDashoffset = GAUGE_CIRCUMFERENCE;

        updateDoorDisplay(false);

        // Khởi tạo Firebase hoặc mô phỏng
        initFirebase();
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }

    // API cho tich hop ben ngoai
    window.ColdVault = {
        updateSensorData,
        setConnectionStatus,
        getState: () => state,
    };

})();
