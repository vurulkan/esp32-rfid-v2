async function fetchJson(url, options) {
  const res = await fetchWithAuth(url, options);
  if (!res || !res.ok) {
    throw new Error(`HTTP ${res.status}`);
  }
  return await res.json();
}

async function fetchWithAuth(url, options) {
  const opts = options || {};
  opts.credentials = 'same-origin';
  const res = await fetch(url, opts);
  if (res.status === 401) {
    window.location = '/login';
  }
  return res;
}

function formatBytes(bytes) {
  if (!bytes || bytes < 0) {
    return '0 KB';
  }
  const kb = Math.round(bytes / 1024);
  return `${kb} KB`;
}

function setBar(barId, textId, free, total) {
  const bar = document.getElementById(barId);
  const text = document.getElementById(textId);
  if (!bar || !text || !total) {
    return;
  }
  const used = Math.max(0, total - free);
  const pct = Math.min(100, Math.round((used / total) * 100));
  bar.style.width = `${pct}%`;
  text.textContent = `${formatBytes(free)} Free / ${formatBytes(total)} Total`;
}

function renderUsers(users) {
  if (!users || users.length === 0) {
    return '<em>No users</em>';
  }
  return users.map(u => {
    return `<div><label class="check"><input type="checkbox" class="user-check" data-uid="${u.uid}"> ${u.uid} - ${u.name} (R1: ${u.relay1 ? 'Y' : 'N'}, R2: ${u.relay2 ? 'Y' : 'N'})</label> <button data-uid="${u.uid}" data-name="${u.name}">Delete</button></div>`;
  }).join('');
}

function renderLogs(logs) {
  if (!logs || logs.length === 0) {
    return '<em>No logs</em>';
  }
  return logs.map(l => `<div>${l.ts} ${l.msg}</div>`).join('');
}

async function loadStatus() {
  const data = await fetchJson('/status');
  const device = data.device || {};
  const memory = data.memory || {};
  const network = data.network || {};

  document.getElementById('device-name').textContent = device.name || '-';
  document.getElementById('device-model').textContent = device.chip_model || '-';
  document.getElementById('device-rev').textContent = device.chip_rev ?? '-';
  document.getElementById('device-cores').textContent = device.cores ?? '-';
  document.getElementById('device-cpu').textContent = device.cpu_mhz ? `${device.cpu_mhz} MHz` : '-';
  document.getElementById('device-uptime').textContent = device.uptime_ms ? `${Math.floor(device.uptime_ms / 1000)} s` : '-';

  setBar('heap-bar', 'heap-text', memory.heap_free, memory.heap_total);
  setBar('flash-bar', 'flash-text', memory.flash_free, memory.flash_total);
  setBar('fs-bar', 'fs-text', memory.littlefs_free, memory.littlefs_total);

  document.getElementById('net-mode').textContent = network.mode || '-';
  document.getElementById('net-ssid').textContent = network.ssid || '-';
  document.getElementById('net-ip').textContent = network.ip || '-';
  document.getElementById('net-gw').textContent = network.gateway || '-';
  document.getElementById('net-mask').textContent = network.mask || '-';
  document.getElementById('net-mac').textContent = network.mac || '-';
}

async function loadUsers() {
  const data = await fetchJson('/users');
  const usersDiv = document.getElementById('users');
  usersDiv.innerHTML = renderUsers(data.users || []);
  usersDiv.querySelectorAll('button[data-uid]').forEach(btn => {
    btn.addEventListener('click', async () => {
      const uid = btn.getAttribute('data-uid');
      const name = btn.getAttribute('data-name') || '';
      const ok = confirm(`Delete user ${uid}${name ? ' - ' + name : ''}?`);
      if (!ok) {
        return;
      }
      await fetchWithAuth(`/users?uid=${encodeURIComponent(uid)}`, { method: 'DELETE', headers: {} });
      await refreshAll();
    });
  });
}

async function loadLogs() {
  const data = await fetchJson('/logs');
  document.getElementById('logs').innerHTML = renderLogs(data.logs || []);
}

async function loadRfid() {
  const data = await fetchJson('/rfid');
  document.getElementById('rfid').textContent = JSON.stringify(data, null, 2);
}

async function loadSettings() {
  const data = await fetchJson('/settings');
  const rtcEnabled = !!data.rtc_enabled;
  const checkbox = document.getElementById('rtc-enabled');
  checkbox.checked = rtcEnabled;
  setRtcConfigVisible(rtcEnabled);
  if (rtcEnabled && data.rtc_time_valid) {
    setRtcStatus('RTC is set.');
  } else if (rtcEnabled) {
    setRtcStatus('RTC not set.');
  } else {
    setRtcStatus('');
  }

  const wifiClient = !!data.wifi_client;
  document.getElementById('wifi-client').checked = wifiClient;
  document.getElementById('wifi-ssid').value = data.wifi_ssid || '';
  document.getElementById('wifi-static').checked = !!data.wifi_static;
  document.getElementById('wifi-ip').value = data.wifi_ip || '';
  document.getElementById('wifi-gateway').value = data.wifi_gateway || '';
  document.getElementById('wifi-mask').value = data.wifi_mask || '';
  document.getElementById('relay1-name').value = data.relay1 || '';
  document.getElementById('relay2-name').value = data.relay2 || '';
  const r1Label = document.getElementById('relay1-label');
  const r2Label = document.getElementById('relay2-label');
  if (r1Label) {
    r1Label.textContent = data.relay1 || 'Relay 1';
  }
  if (r2Label) {
    r2Label.textContent = data.relay2 || 'Relay 2';
  }
  const maintR1 = document.getElementById('maint-relay1-label');
  const maintR2 = document.getElementById('maint-relay2-label');
  if (maintR1) {
    maintR1.textContent = data.relay1 || 'Relay 1';
  }
  if (maintR2) {
    maintR2.textContent = data.relay2 || 'Relay 2';
  }
  const relay1State = document.getElementById('relay1-state-text');
  const relay2State = document.getElementById('relay2-state-text');
  if (relay1State) {
    relay1State.textContent = `${data.relay1 || 'Relay 1'}: ${data.relay1_state ? 'ON' : 'OFF'}`;
  }
  if (relay2State) {
    relay2State.textContent = `${data.relay2 || 'Relay 2'}: ${data.relay2_state ? 'ON' : 'OFF'}`;
  }
  const relay1Toggle = document.getElementById('relay1-toggle');
  const relay2Toggle = document.getElementById('relay2-toggle');
  if (relay1Toggle) {
    relay1Toggle.checked = !!data.relay1_state;
  }
  if (relay2Toggle) {
    relay2Toggle.checked = !!data.relay2_state;
  }
  setWifiClientVisible(wifiClient);
  setWifiStaticVisible(!!data.wifi_static);
  document.getElementById('auth-enabled').checked = !!data.auth_enabled;
  document.getElementById('auth-user').value = data.auth_user || '';
  const apiKeyDisplay = document.getElementById('api-key-display');
  if (apiKeyDisplay) {
    apiKeyDisplay.textContent = data.api_key_mask ? `API key: ${data.api_key_mask}` : '';
  }
  setAuthFieldsVisible(!!data.auth_enabled);
  const logoutButton = document.getElementById('logout');
  if (logoutButton) {
    logoutButton.classList.toggle('is-hidden', !data.auth_enabled);
  }
}

function setRtcConfigVisible(enabled) {
  const section = document.getElementById('rtc-config');
  if (!section) {
    return;
  }
  section.classList.toggle('is-hidden', !enabled);
}

function setRtcStatus(text) {
  const el = document.getElementById('rtc-status');
  if (el) {
    el.textContent = text || '';
  }
}

function setWifiClientVisible(enabled) {
  const section = document.getElementById('wifi-client-fields');
  if (!section) {
    return;
  }
  section.classList.toggle('is-hidden', !enabled);
}

function setWifiStaticVisible(enabled) {
  const section = document.getElementById('wifi-static-fields');
  if (!section) {
    return;
  }
  section.classList.toggle('is-hidden', !enabled);
}

function setAuthFieldsVisible(enabled) {
  const section = document.getElementById('auth-fields');
  if (!section) {
    return;
  }
  section.classList.toggle('is-hidden', !enabled);
  const hint = document.getElementById('auth-hint');
  if (hint) {
    hint.classList.toggle('is-hidden', !enabled);
  }
  const display = document.getElementById('api-key-display');
  if (display) {
    display.classList.toggle('is-hidden', !enabled);
    if (!enabled) {
      display.textContent = '';
    }
  }
}

function getPulseDuration() {
  const input = document.getElementById('relay-pulse-ms');
  if (!input) {
    return '600';
  }
  const value = parseInt(input.value, 10);
  if (Number.isNaN(value) || value < 50) {
    return '50';
  }
  if (value > 10000) {
    return '10000';
  }
  return String(value);
}

async function setRelayState(relayId, enabled, confirmToggle) {
  if (confirmToggle) {
    const label = relayId === 1 ? 'Relay 1' : 'Relay 2';
    const ok = confirm(`${enabled ? 'Turn on' : 'Turn off'} ${label}?`);
    if (!ok) {
      return;
    }
  }
  const params = new URLSearchParams();
  params.set('relay', String(relayId));
  params.set('action', enabled ? 'on' : 'off');
  await fetchWithAuth('/maintenance/relay', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: params.toString()
  });
  await loadSettings();
}

async function fetchLastUid() {
  const data = await fetchJson('/rfid');
  const uid = data && data.rfid ? data.rfid.uid : '';
  if (!uid) {
    throw new Error('no uid');
  }
  return uid;
}

async function refreshAll() {
  await Promise.all([loadStatus(), loadUsers(), loadLogs(), loadRfid(), loadSettings()]);
}

const menuItems = document.querySelectorAll('.menu-item');
function isPageActive(name) {
  const page = document.getElementById(`page-${name}`);
  return page && page.classList.contains('is-active');
}

menuItems.forEach(btn => {
  btn.addEventListener('click', () => {
    menuItems.forEach(b => b.classList.remove('is-active'));
    btn.classList.add('is-active');
    const target = btn.getAttribute('data-target');
    document.querySelectorAll('.page').forEach(page => {
      page.classList.toggle('is-active', page.id === `page-${target}`);
    });
    if (target === 'logs') {
      loadLogs();
    }
    if (target === 'settings') {
      loadSettings();
    }
    if (target === 'backup') {
      const status = document.getElementById('restore-status');
      if (status) {
        status.textContent = '';
      }
    }
  });
});

const form = document.getElementById('user-form');
form.addEventListener('submit', async (e) => {
  e.preventDefault();
  const formData = new FormData(form);
  let uid = (formData.get('uid') || '').toString().trim();
  if (!uid) {
    try {
      uid = await fetchLastUid();
    } catch (err) {
      alert('Card not scanned. Scan a card first.');
      return;
    }
  }
  const params = new URLSearchParams();
  params.set('uid', uid);
  params.set('name', formData.get('name'));
  params.set('relay1', formData.get('relay1') ? '1' : '0');
  params.set('relay2', formData.get('relay2') ? '1' : '0');
  const res = await fetchWithAuth('/users', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: params.toString()
  });
  let payload = {};
  try {
    payload = await res.json();
  } catch (err) {
    payload = {};
  }
  if (!res.ok || payload.ok === false) {
    if (payload.error === 'uid_exists') {
      alert('This UID is already registered.');
    } else {
      alert('Save failed.');
    }
    return;
  }
  form.reset();
  await refreshAll();
});

document.getElementById('use-last').addEventListener('click', async () => {
  try {
    const uid = await fetchLastUid();
    form.querySelector('input[name="uid"]').value = uid;
  } catch (err) {
    alert('Card not scanned. Scan a card first.');
  }
});

document.getElementById('select-all-users').addEventListener('click', () => {
  document.querySelectorAll('.user-check').forEach(ch => {
    ch.checked = true;
  });
});

document.getElementById('deselect-all-users').addEventListener('click', () => {
  document.querySelectorAll('.user-check').forEach(ch => {
    ch.checked = false;
  });
});

document.getElementById('delete-selected-users').addEventListener('click', async () => {
  const selected = Array.from(document.querySelectorAll('.user-check'))
    .filter(ch => ch.checked)
    .map(ch => ch.getAttribute('data-uid'));
  if (selected.length === 0) {
    alert('Select at least one user.');
    return;
  }
  const ok = confirm(`Delete ${selected.length} users?`);
  if (!ok) {
    return;
  }
  for (const uid of selected) {
    await fetchWithAuth(`/users?uid=${encodeURIComponent(uid)}`, { method: 'DELETE', headers: {} });
  }
  await refreshAll();
});

document.getElementById('clear-ram-logs').addEventListener('click', async () => {
  const ok = confirm('Clear last 50 RAM logs?');
  if (!ok) {
    return;
  }
    await fetchWithAuth('/logs?scope=ram', { method: 'DELETE', headers: {} });
  await refreshAll();
});

document.getElementById('clear-all-logs').addEventListener('click', async () => {
  const ok = confirm('Clear all logs in LittleFS?');
  if (!ok) {
    return;
  }
    await fetchWithAuth('/logs?scope=all', { method: 'DELETE', headers: {} });
  await refreshAll();
});

document.getElementById('download-logs').addEventListener('click', async () => {
  const res = await fetchWithAuth('/logs/export', { headers: {} });
  if (!res.ok) {
    alert('Download failed.');
    return;
  }
  const text = await res.text();
  const blob = new Blob([text], { type: 'text/plain' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = 'logs.txt';
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
});

document.getElementById('format-fs').addEventListener('click', async () => {
  const ok = confirm('Format LittleFS? All records will be deleted.');
  if (!ok) {
    return;
  }
  try {
    await fetchWithAuth('/maintenance/format', { method: 'POST', headers: {} });
    alert('Format completed. Reboot the device if needed.');
    await refreshAll();
  } catch (err) {
    alert('Format failed.');
  }
});

document.getElementById('reboot-device').addEventListener('click', async () => {
  const ok = confirm('Reboot the device now?');
  if (!ok) {
    return;
  }
  try {
    await fetchWithAuth('/maintenance/reboot', { method: 'POST', headers: {} });
    alert('Rebooting...');
  } catch (err) {
    alert('Reboot failed.');
  }
});

document.getElementById('rtc-enabled').addEventListener('change', (e) => {
  setRtcConfigVisible(e.target.checked);
});

document.getElementById('rtc-save').addEventListener('click', async () => {
  const enabled = document.getElementById('rtc-enabled').checked;
  const params = new URLSearchParams();
  params.set('rtc_enabled', enabled ? '1' : '0');
  try {
    await fetchWithAuth('/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: params.toString()
    });
    setRtcStatus(enabled ? 'RTC enabled.' : 'RTC disabled.');
  } catch (err) {
    setRtcStatus('Failed to update settings.');
  }
});

document.getElementById('rtc-set').addEventListener('click', async () => {
  const input = document.getElementById('rtc-datetime');
  if (!input.value) {
    setRtcStatus('Select a date/time first.');
    return;
  }
  const params = new URLSearchParams();
  params.set('datetime', input.value);
  try {
    const res = await fetchWithAuth('/rtc', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: params.toString()
    });
    const payload = await res.json();
    if (!res.ok || payload.ok === false) {
      setRtcStatus('RTC update failed.');
      return;
    }
    setRtcStatus('RTC updated.');
  } catch (err) {
    setRtcStatus('RTC update failed.');
  }
});

document.getElementById('rtc-read').addEventListener('click', async () => {
  try {
    const data = await fetchJson('/rtc');
    if (data.datetime) {
      const iso = data.datetime.replace(' ', 'T').slice(0, 16);
      document.getElementById('rtc-datetime').value = iso;
      setRtcStatus('RTC read successful.');
    }
  } catch (err) {
    setRtcStatus('RTC read failed.');
  }
});

document.getElementById('wifi-save').addEventListener('click', async () => {
  const enabled = document.getElementById('wifi-client').checked;
  const ssid = document.getElementById('wifi-ssid').value.trim();
  const pass = document.getElementById('wifi-pass').value;
  const useStatic = document.getElementById('wifi-static').checked;
  const ip = document.getElementById('wifi-ip').value.trim();
  const gateway = document.getElementById('wifi-gateway').value.trim();
  const mask = document.getElementById('wifi-mask').value.trim();
  if (enabled && !ssid) {
    alert('SSID is required for client mode.');
    return;
  }
  if (useStatic && (!ip || !gateway || !mask)) {
    alert('Static IP requires IP, Gateway, and Mask.');
    return;
  }
  const params = new URLSearchParams();
  params.set('wifi_client', enabled ? '1' : '0');
  params.set('wifi_ssid', ssid);
  params.set('wifi_pass', pass);
  params.set('wifi_static', useStatic ? '1' : '0');
  params.set('wifi_ip', ip);
  params.set('wifi_gateway', gateway);
  params.set('wifi_mask', mask);
  try {
    const res = await fetchWithAuth('/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: params.toString()
    });
    let payload = {};
    try {
      payload = await res.json();
    } catch (err) {
      payload = {};
    }
    if (payload.reboot) {
      alert('WiFi settings saved. Reboot the device to switch mode.');
    } else {
      alert('WiFi settings saved.');
    }
  } catch (err) {
    alert('Failed to save WiFi settings.');
  }
});

document.getElementById('save-relay-names').addEventListener('click', async () => {
  const relay1 = document.getElementById('relay1-name').value.trim();
  const relay2 = document.getElementById('relay2-name').value.trim();
  const params = new URLSearchParams();
  params.set('relay1', relay1);
  params.set('relay2', relay2);
  try {
    await fetchWithAuth('/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: params.toString()
    });
    alert('Relay names saved.');
    await loadSettings();
  } catch (err) {
    alert('Failed to save relay names.');
  }
});

document.getElementById('trigger-relay1').addEventListener('click', async () => {
  const ok = confirm('Pulse Relay 1?');
  if (!ok) {
    return;
  }
  const params = new URLSearchParams();
  params.set('relay', '1');
  params.set('action', 'pulse');
  params.set('duration_ms', getPulseDuration());
  await fetchWithAuth('/maintenance/relay', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: params.toString()
  });
});

document.getElementById('trigger-relay2').addEventListener('click', async () => {
  const ok = confirm('Pulse Relay 2?');
  if (!ok) {
    return;
  }
  const params = new URLSearchParams();
  params.set('relay', '2');
  params.set('action', 'pulse');
  params.set('duration_ms', getPulseDuration());
  await fetchWithAuth('/maintenance/relay', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: params.toString()
  });
});

document.getElementById('relay1-toggle').addEventListener('change', async (e) => {
  await setRelayState(1, e.target.checked, false);
});

document.getElementById('relay2-toggle').addEventListener('change', async (e) => {
  await setRelayState(2, e.target.checked, false);
});

const uartTestButton = document.getElementById('uart-test');
if (uartTestButton) {
  uartTestButton.addEventListener('click', async () => {
    const status = document.getElementById('uart-test-status');
    if (status) {
      status.textContent = 'Testing UART link...';
    }
    const res = await fetchWithAuth('/maintenance/uart-test', { method: 'POST' });
    let ok = false;
    try {
      const payload = await res.json();
      ok = payload && payload.ok === true;
    } catch (err) {
      ok = false;
    }
    if (status) {
      status.textContent = ok ? 'Nano link OK.' : 'No response from Nano.';
    }
  });
}

function bindReaderTest(buttonId, readerId, action) {
  const button = document.getElementById(buttonId);
  if (!button) {
    return;
  }
  button.addEventListener('click', async () => {
    const params = new URLSearchParams();
    params.set('reader', String(readerId));
    params.set('action', action);
    await fetchWithAuth('/maintenance/reader-test', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: params.toString()
    });
  });
}

bindReaderTest('reader1-test-allow', 1, 'allow');
bindReaderTest('reader1-test-deny', 1, 'deny');
bindReaderTest('reader2-test-allow', 2, 'allow');
bindReaderTest('reader2-test-deny', 2, 'deny');

document.getElementById('save-auth').addEventListener('click', async () => {
  const enabled = document.getElementById('auth-enabled').checked;
  const user = document.getElementById('auth-user').value.trim();
  const pass = document.getElementById('auth-pass').value;
  if (enabled && (!user || !pass)) {
    alert('Username and password are required.');
    return;
  }
  const params = new URLSearchParams();
  params.set('auth_enabled', enabled ? '1' : '0');
  params.set('auth_user', user);
  params.set('auth_pass', pass);
  try {
    const res = await fetchWithAuth('/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: params.toString()
    });
    const payload = await res.json();
    if (payload.api_key) {
      const display = document.getElementById('api-key-display');
      if (display) {
        display.textContent = `API key: ${payload.api_key}`;
      }
      alert('Authentication enabled. Save the API key now.');
    } else {
      alert('Authentication settings saved.');
      await loadSettings();
    }
  } catch (err) {
    alert('Failed to save authentication settings.');
  }
});

document.getElementById('wifi-client').addEventListener('change', (e) => {
  setWifiClientVisible(e.target.checked);
});

document.getElementById('wifi-static').addEventListener('change', (e) => {
  setWifiStaticVisible(e.target.checked);
});

document.getElementById('auth-enabled').addEventListener('change', (e) => {
  setAuthFieldsVisible(e.target.checked);
});

const logoutButton = document.getElementById('logout');
if (logoutButton) {
  logoutButton.addEventListener('click', async () => {
    await fetchWithAuth('/auth/logout', { method: 'POST' });
    window.location = '/login';
  });
}

async function triggerBackup(type) {
  const res = await fetchWithAuth(`/backup?type=${encodeURIComponent(type)}`, { headers: {} });
  if (!res.ok) {
    alert('Backup failed.');
    return;
  }
  const text = await res.text();
  const blob = new Blob([text], { type: 'text/plain' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = `backup-${type}.txt`;
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}

document.getElementById('backup-users').addEventListener('click', () => {
  triggerBackup('users');
});

document.getElementById('backup-settings').addEventListener('click', () => {
  triggerBackup('settings');
});


document.getElementById('restore-apply').addEventListener('click', async () => {
  const fileInput = document.getElementById('restore-file');
  const status = document.getElementById('restore-status');
  if (!fileInput.files || fileInput.files.length === 0) {
    status.textContent = 'Select a backup file first.';
    return;
  }
  const file = fileInput.files[0];
  const data = await file.text();
  try {
    const res = await fetchWithAuth('/restore', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: data
    });
    const payload = await res.json();
    if (!res.ok || payload.ok === false) {
      status.textContent = 'Restore failed.';
      return;
    }
    status.textContent = 'Restore complete. Reboot if needed.';
    await refreshAll();
  } catch (err) {
    status.textContent = 'Restore failed.';
  }
});

refreshAll();
setInterval(loadStatus, 2000);
setInterval(loadRfid, 1500);
setInterval(() => {
  if (isPageActive('logs')) {
    loadLogs();
  }
}, 1200);

window.addEventListener('beforeunload', () => {
  clearApiKey();
});
