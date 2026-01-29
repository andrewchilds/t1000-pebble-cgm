/**
 * T1000 CGM Watchface - PebbleKit JS
 *
 * Handles Dexcom Share authentication, data fetching, and smart polling.
 * Polls at 5 minutes + 45 seconds after last good reading to minimize battery usage.
 */

// Import Clay for configuration
var Clay = require("pebble-clay");
var clayConfig = require("./config");
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

// AppMessage keys (must match appinfo.json and main.c)
var KEY_CGM_VALUE = 0;
var KEY_CGM_DELTA = 1;
var KEY_CGM_TREND = 2;
var KEY_CGM_TIME_AGO = 3;
var KEY_CGM_HISTORY = 4;
var KEY_CGM_ALERT = 5;
var KEY_REQUEST_DATA = 6;
var KEY_LOW_THRESHOLD = 7;
var KEY_HIGH_THRESHOLD = 8;
var KEY_NEEDS_SETUP = 9;
var KEY_REVERSED = 10;
var KEY_SYNC_ERROR = 11;

// Dexcom Share API endpoints
var DEXCOM_URLS = {
	us: "https://share1.dexcom.com",
	international: "https://shareous1.dexcom.com"
};

// Dexcom application ID (same as official Dexcom app uses)
var DEXCOM_APP_ID = "d89443d2-327c-4a6f-89e5-496bbb0317db";

// Trend direction mapping (Dexcom values)
var TREND_DIRECTIONS = {
	None: 0,
	DoubleUp: 1,
	SingleUp: 2,
	FortyFiveUp: 3,
	Flat: 4,
	FortyFiveDown: 5,
	SingleDown: 6,
	DoubleDown: 7,
	"NOT COMPUTABLE": 0,
	"RATE OUT OF RANGE": 0
};

// State
var sessionId = null;
var lastGoodReadingTime = null;
var pollTimer = null;
var settings = {
	accountName: "",
	password: "",
	server: "us",
	unit: "mgdl",
	reversed: false,
	highThreshold: 180,
	lowThreshold: 70,
	vibeLowSoonEnabled: false,
	vibeLowSoonThreshold: 80,
	vibeLowSoonRepeatMinutes: 30,
	vibeEnabled: false,
	vibeHighThreshold: 250,
	vibeDelayMinutes: 60,
	vibeRepeatMinutes: 60
};

// Vibration state (persisted to localStorage to survive app restarts)
var vibeHighConditionStartTime = null;
var lastHighVibeTime = null;
var lastLowSoonVibeTime = null;

/**
 * Load persisted vibration state from localStorage
 */
function loadVibeState() {
	var stored = localStorage.getItem("vibe-state");
	if (stored) {
		try {
			var parsed = JSON.parse(stored);
			vibeHighConditionStartTime = parsed.vibeHighConditionStartTime || null;
			lastHighVibeTime = parsed.lastHighVibeTime || null;
			lastLowSoonVibeTime = parsed.lastLowSoonVibeTime || null;
			console.log(
				"Vibe state loaded: highStart=" +
					vibeHighConditionStartTime +
					", lastHigh=" +
					lastHighVibeTime +
					", lastLowSoon=" +
					lastLowSoonVibeTime
			);
		} catch (e) {
			console.log("Error parsing vibe state: " + e);
		}
	}
}

/**
 * Save vibration state to localStorage
 */
function saveVibeState() {
	var state = {
		vibeHighConditionStartTime: vibeHighConditionStartTime,
		lastHighVibeTime: lastHighVibeTime,
		lastLowSoonVibeTime: lastLowSoonVibeTime
	};
	localStorage.setItem("vibe-state", JSON.stringify(state));
}

/**
 * Cache CGM readings to localStorage
 */
function cacheReadings(readings) {
	if (!readings || readings.length === 0) {
		return;
	}
	var cache = {
		readings: readings,
		cachedAt: Date.now()
	};
	localStorage.setItem("cgm-cache", JSON.stringify(cache));
	console.log("Cached " + readings.length + " readings");
}

/**
 * Get cached readings if still valid (latest reading is less than 5 minutes old)
 * Returns null if cache is invalid or stale
 */
function getCachedReadings() {
	var stored = localStorage.getItem("cgm-cache");
	if (!stored) {
		return null;
	}

	try {
		var cache = JSON.parse(stored);
		if (!cache.readings || cache.readings.length === 0) {
			return null;
		}

		// Check if the latest reading's timestamp is less than 5 minutes old
		var latestTimestamp = parseDexcomTimestamp(cache.readings[0].WT);
		if (!latestTimestamp) {
			return null;
		}

		var now = Date.now();
		var ageMs = now - latestTimestamp;
		var ageMinutes = ageMs / 60000;

		if (ageMinutes < 5) {
			console.log("Using cached readings (latest is " + ageMinutes.toFixed(1) + " min old)");
			return cache.readings;
		} else {
			console.log("Cache stale (latest is " + ageMinutes.toFixed(1) + " min old)");
			return null;
		}
	} catch (e) {
		console.log("Error parsing CGM cache: " + e);
		return null;
	}
}

// Alert types to send to watch
var ALERT_NONE = 0;
var ALERT_LOW_SOON = 1;
var ALERT_HIGH = 2;
var pendingAlert = ALERT_NONE;

/**
 * Load settings from localStorage (Clay format)
 */
function loadSettings() {
	var stored = localStorage.getItem("clay-settings");
	if (stored) {
		try {
			var parsed = JSON.parse(stored);
			for (var key in parsed) {
				if (settings.hasOwnProperty(key) && parsed[key] !== undefined) {
					settings[key] = parsed[key];
				}
			}
			console.log("Settings loaded");
		} catch (e) {
			console.log("Error parsing settings: " + e);
		}
	}
}

/**
 * Save settings to localStorage (Clay format)
 */
function saveSettings() {
	localStorage.setItem("clay-settings", JSON.stringify(settings));
}

/**
 * Get the Dexcom Share base URL based on settings
 */
function getDexcomBaseUrl() {
	return DEXCOM_URLS[settings.server] || DEXCOM_URLS.us;
}

/**
 * Convert mg/dL to mmol/L
 */
function mgdlToMmol(mgdl) {
	return (mgdl / 18.0182).toFixed(1);
}

/**
 * Format glucose value based on unit setting
 */
function formatGlucose(mgdl) {
	if (mgdl < 40) {
		return "LOW";
	}
	if (mgdl > 400) {
		return "HIGH";
	}
	if (settings.unit === "mmol") {
		return mgdlToMmol(mgdl);
	}
	return mgdl.toString();
}

/**
 * Format delta value based on unit setting
 */
function formatDelta(deltaMgdl) {
	var formatted;
	if (settings.unit === "mmol") {
		formatted = (deltaMgdl / 18.0182).toFixed(1);
	} else {
		formatted = Math.round(deltaMgdl).toString();
	}
	if (deltaMgdl >= 0) {
		return "+" + formatted;
	}
	return formatted;
}

/**
 * Make HTTP request with promise
 */
function httpRequest(method, url, body, headers) {
	return new Promise(function (resolve, reject) {
		var xhr = new XMLHttpRequest();
		xhr.open(method, url, true);

		// Set headers
		xhr.setRequestHeader("Content-Type", "application/json");
		xhr.setRequestHeader("Accept", "application/json");
		xhr.setRequestHeader("User-Agent", "Dexcom Share/3.0.2.11 CFNetwork/711.2.23 Darwin/14.0.0");

		if (headers) {
			for (var key in headers) {
				xhr.setRequestHeader(key, headers[key]);
			}
		}

		xhr.onload = function () {
			if (xhr.status >= 200 && xhr.status < 300) {
				try {
					var response = JSON.parse(xhr.responseText);
					resolve(response);
				} catch (e) {
					// Response might be a plain string (like session ID)
					resolve(xhr.responseText.replace(/"/g, ""));
				}
			} else {
				reject(new Error("HTTP " + xhr.status + ": " + xhr.statusText));
			}
		};

		xhr.onerror = function () {
			reject(new Error("Network error"));
		};

		xhr.ontimeout = function () {
			reject(new Error("Request timeout"));
		};

		xhr.timeout = 30000; // 30 second timeout

		if (body) {
			xhr.send(JSON.stringify(body));
		} else {
			xhr.send();
		}
	});
}

/**
 * Authenticate with Dexcom Share
 */
function dexcomLogin() {
	var baseUrl = getDexcomBaseUrl();
	var url = baseUrl + "/ShareWebServices/Services/General/LoginPublisherAccountByName";

	console.log("Logging in to Dexcom Share...");

	return httpRequest("POST", url, {
		accountName: settings.accountName,
		password: settings.password,
		applicationId: DEXCOM_APP_ID
	}).then(function (response) {
		sessionId = response;
		console.log("Login successful, session: " + sessionId.substring(0, 8) + "...");
		return sessionId;
	});
}

/**
 * Fetch glucose readings from Dexcom Share
 */
function dexcomFetchReadings() {
	if (!sessionId) {
		return Promise.reject(new Error("Not logged in"));
	}

	var baseUrl = getDexcomBaseUrl();
	// Fetch 24 readings for 120 minutes of data (24 * 5 = 120)
	var url =
		baseUrl +
		"/ShareWebServices/Services/Publisher/ReadPublisherLatestGlucoseValues" +
		"?sessionID=" +
		encodeURIComponent(sessionId) +
		"&minutes=1440" +
		"&maxCount=24";

	console.log("Fetching glucose readings...");

	return httpRequest("POST", url, null);
}

/**
 * Parse Dexcom timestamp
 * Format: "/Date(1234567890000)/"
 */
function parseDexcomTimestamp(dtString) {
	var match = dtString.match(/Date\((\d+)\)/);
	if (match) {
		return parseInt(match[1], 10);
	}
	return null;
}

/**
 * Process glucose readings and send to watch
 */
function processReadings(readings, fromCache) {
	if (!readings || readings.length === 0) {
		console.log("No readings received");
		sendError("No data");
		return;
	}

	// Cache fresh readings from the API
	if (!fromCache) {
		cacheReadings(readings);
	}

	console.log("Processing " + readings.length + " readings" + (fromCache ? " (from cache)" : ""));

	// Most recent reading
	var latest = readings[0];
	var latestValue = latest.Value;
	var latestTimestamp = parseDexcomTimestamp(latest.WT);
	var latestTrendString = latest.Trend || "None";
	var latestTrend = TREND_DIRECTIONS[latestTrendString] || 0;

	// Handle numeric trend values from API
	if (typeof latestTrendString === "number") {
		latestTrend = latestTrendString > 7 ? 0 : latestTrendString;
	}

	// Calculate time ago
	var now = Date.now();
	var minutesAgo = Math.round((now - latestTimestamp) / 60000);

	// Calculate delta (difference from previous reading)
	var delta = 0;
	if (readings.length > 1) {
		var previousValue = readings[1].Value;
		var previousTimestamp = parseDexcomTimestamp(readings[1].WT);
		var timeDiffMinutes = (latestTimestamp - previousTimestamp) / 60000;

		// Normalize to 5-minute rate
		if (timeDiffMinutes > 0) {
			delta = ((latestValue - previousValue) / timeDiffMinutes) * 5;
		}
	}

	// Build history string (value:minutesAgo pairs, most recent first)
	// Format: "120:0,125:5,130:10" where second number is minutes ago from now
	var history = readings
		.map(function (r) {
			var timestamp = parseDexcomTimestamp(r.WT);
			var minutesAgo = Math.round((now - timestamp) / 60000);
			return r.Value + ":" + minutesAgo;
		})
		.join(",");

	// Update last good reading time for smart polling
	lastGoodReadingTime = latestTimestamp;

	// Check vibration conditions (sets pendingAlert if needed)
	pendingAlert = ALERT_NONE;
	checkLowSoonAlert(readings);
	checkVibrationAlert(latestValue);

	// Send data to watch
	var message = {};
	message[KEY_CGM_VALUE] = formatGlucose(latestValue);
	message[KEY_CGM_DELTA] = formatDelta(delta);
	message[KEY_CGM_TREND] = latestTrend;
	message[KEY_CGM_TIME_AGO] = minutesAgo;
	message[KEY_CGM_HISTORY] = history;
	message[KEY_CGM_ALERT] = pendingAlert;
	message[KEY_LOW_THRESHOLD] = settings.lowThreshold;
	message[KEY_HIGH_THRESHOLD] = settings.highThreshold;
	message[KEY_REVERSED] = settings.reversed ? 1 : 0;
	message[KEY_NEEDS_SETUP] = 0;
	message[KEY_SYNC_ERROR] = 0; // Success - no sync error

	console.log(
		"Sending: value=" +
			latestValue +
			" (" +
			formatGlucose(latestValue) +
			"), " +
			"delta=" +
			formatDelta(delta) +
			", trend=" +
			latestTrend +
			", " +
			"ago=" +
			minutesAgo +
			"min, history=" +
			readings.length +
			" points"
	);

	Pebble.sendAppMessage(
		message,
		function () {
			console.log("Data sent to watch");
		},
		function (e) {
			console.log("Error sending data: " + JSON.stringify(e));
		}
	);

	// Schedule next poll
	scheduleNextPoll();
}

/**
 * Calculate weighted average velocity from recent readings
 * Uses multiple time spans with heavier weighting on recent changes
 * Returns velocity in mg/dL per 5 minutes, or null if insufficient data or gaps detected
 */
function calculateVelocity(readings) {
	// Need at least 5 readings to calculate velocity
	if (!readings || readings.length < 5) {
		return null;
	}

	// Check that the first 5 values are valid (non-zero) and have no gaps
	// Each reading should be ~5 minutes apart; allow up to 7 minutes to account for slight delays
	var maxGapMs = 7 * 60 * 1000; // 7 minutes in milliseconds
	for (var i = 0; i < 5; i++) {
		if (!readings[i] || readings[i].Value === 0) {
			return null;
		}
		// Check gap between consecutive readings (except for the last one)
		if (i < 4) {
			var thisTime = parseDexcomTimestamp(readings[i].WT);
			var nextTime = parseDexcomTimestamp(readings[i + 1].WT);
			if (!thisTime || !nextTime) {
				return null;
			}
			var gap = thisTime - nextTime; // readings are most-recent-first
			if (gap > maxGapMs) {
				console.log(
					"Velocity calculation skipped: gap of " +
						Math.round(gap / 60000) +
						" min between readings " +
						i +
						" and " +
						(i + 1)
				);
				return null;
			}
		}
	}

	var bg0 = readings[0].Value;
	var bg1 = readings[1].Value;
	var bg2 = readings[2].Value;
	var bg3 = readings[3].Value;
	var bg4 = readings[4].Value;

	// Weigh newer values more heavily
	var w1 = 0.29;
	var w2 = 0.27;
	var w3 = 0.23;
	var w4 = 0.21;

	// Calculate velocities over different time spans (per 5-minute interval)
	var vel1 = bg0 - bg1; // 5-minute change
	var vel2 = (bg0 - bg2) / 2.0; // 10-minute change, normalized to 5-min
	var vel3 = (bg0 - bg3) / 3.0; // 15-minute change, normalized to 5-min
	var vel4 = (bg0 - bg4) / 4.0; // 20-minute change, normalized to 5-min

	// Calculate the weighted average velocity
	var velocity = vel1 * w1 + vel2 * w2 + vel3 * w3 + vel4 * w4;

	console.log("Weighted average velocity: " + velocity.toFixed(1) + " mg/dL per 5min");

	return velocity;
}

/**
 * Check if "low soon" alert should trigger based on predicted value in 20 minutes
 * Uses weighted average velocity from recent readings for smoother prediction
 */
function checkLowSoonAlert(readings) {
	if (!settings.vibeLowSoonEnabled) {
		return;
	}

	var velocity = calculateVelocity(readings);
	if (velocity === null) {
		console.log("Low soon alert: insufficient data for velocity calculation");
		return;
	}

	var currentValue = readings[0].Value;
	// velocity is per 5 minutes, so multiply by 4 to get 20-minute prediction
	var predictedValue = currentValue + velocity * 4;
	var isLowSoon = predictedValue < settings.vibeLowSoonThreshold;
	var now = Date.now();

	if (isLowSoon) {
		// Check if we should vibrate (first time or repeat interval passed)
		var shouldVibe = false;
		if (!lastLowSoonVibeTime) {
			shouldVibe = true;
		} else {
			var timeSinceVibe = (now - lastLowSoonVibeTime) / 60000; // minutes
			if (timeSinceVibe >= settings.vibeLowSoonRepeatMinutes) {
				shouldVibe = true;
			}
		}

		if (shouldVibe) {
			console.log(
				"Triggering low soon alert vibration (current: " +
					currentValue +
					", predicted: " +
					Math.round(predictedValue) +
					" in 20min)"
			);
			pendingAlert = ALERT_LOW_SOON;
			lastLowSoonVibeTime = now;
			saveVibeState();
		}
	}
	// Note: We intentionally do NOT reset lastLowSoonVibeTime when the condition clears.
	// This ensures that if the user briefly rises above the threshold and then falls back,
	// they won't get repeated alerts within the repeat interval.
}

/**
 * Check if vibration alert should trigger
 */
function checkVibrationAlert(value) {
	if (!settings.vibeEnabled) {
		return;
	}

	var isHighAlert = value >= settings.vibeHighThreshold;
	var now = Date.now();

	if (isHighAlert) {
		// Start tracking condition if not already
		if (!vibeHighConditionStartTime) {
			vibeHighConditionStartTime = now;
		}

		var conditionDuration = (now - vibeHighConditionStartTime) / 60000; // minutes

		// Check if delay has passed
		if (conditionDuration >= settings.vibeDelayMinutes) {
			// Check if we should vibrate (first time or repeat interval passed)
			var shouldVibe = false;
			if (!lastHighVibeTime) {
				shouldVibe = true;
			} else {
				var timeSinceVibe = (now - lastHighVibeTime) / 60000; // minutes
				if (timeSinceVibe >= settings.vibeRepeatMinutes) {
					shouldVibe = true;
				}
			}

			if (shouldVibe) {
				console.log("Triggering high alert vibration");
				pendingAlert = ALERT_HIGH;
				lastHighVibeTime = now;
				saveVibeState();
			}
		}
	} else {
		// Reset condition start time when condition clears (so delay timer restarts)
		// but keep lastHighVibeTime to prevent repeated alerts within repeat interval
		if (vibeHighConditionStartTime !== null) {
			vibeHighConditionStartTime = null;
			saveVibeState();
		}
	}
}

/**
 * Send error message to watch
 */
function sendError(errorText, needsSetup) {
	var message = {};
	message[KEY_CGM_VALUE] = "";
	message[KEY_CGM_DELTA] = "";
	message[KEY_CGM_TREND] = 0;
	message[KEY_CGM_TIME_AGO] = 0;
	message[KEY_NEEDS_SETUP] = needsSetup ? 1 : 0;
	// Signal sync error unless this is just a setup issue
	message[KEY_SYNC_ERROR] = needsSetup ? 0 : 1;

	Pebble.sendAppMessage(
		message,
		function () {
			console.log("Error sent to watch", errorText);
		},
		function (e) {
			console.log("Failed to send error: " + JSON.stringify(e));
		}
	);
}

/**
 * Main fetch function - authenticate if needed, then fetch data
 */
function fetchData() {
	if (!settings.accountName || !settings.password) {
		console.log("No credentials configured");
		sendError("Setup", true);
		return;
	}

	// Check cache first - use cached data if latest reading is less than 5 minutes old
	var cachedReadings = getCachedReadings();
	if (cachedReadings) {
		processReadings(cachedReadings, true);
		return;
	}

	// If we have a session, try to fetch directly
	if (sessionId) {
		dexcomFetchReadings()
			.then(processReadings)
			.catch(function (error) {
				console.log("Fetch failed, re-authenticating: " + error.message);
				// Session might be expired, try re-auth
				sessionId = null;
				dexcomLogin()
					.then(dexcomFetchReadings)
					.then(processReadings)
					.catch(function (error) {
						console.log("Re-auth failed: " + error.message);
						sendError("Auth err");
					});
			});
	} else {
		// Need to login first
		dexcomLogin()
			.then(dexcomFetchReadings)
			.then(processReadings)
			.catch(function (error) {
				console.log("Login/fetch failed: " + error.message);
				if (error.message.indexOf("401") >= 0 || error.message.indexOf("500") >= 0) {
					sendError("Auth err");
				} else {
					sendError("Net err");
				}
			});
	}
}

/**
 * Schedule next poll based on smart timing
 * Poll at 5 minutes + 45 seconds after last good reading
 */
function scheduleNextPoll() {
	// Clear any existing timer
	if (pollTimer) {
		clearTimeout(pollTimer);
		pollTimer = null;
	}

	if (!lastGoodReadingTime) {
		// No good reading yet, poll every 30 seconds
		pollTimer = setTimeout(fetchData, 30000);
		console.log("No reading yet, polling in 30s");
		return;
	}

	var now = Date.now();

	// Expected next reading: 5 minutes after last reading
	// We poll at 5 minutes + 30 seconds to give Dexcom time to process
	var pollInterval = (5 * 60 + 30) * 1000; // 5m 30s in milliseconds
	var nextPollTime = lastGoodReadingTime + pollInterval;

	// If we've already passed the next poll time, calculate the one after
	while (nextPollTime <= now) {
		nextPollTime += pollInterval;
	}

	var delay = nextPollTime - now;

	// Cap at 6 minutes max (in case of drift)
	if (delay > 6 * 60 * 1000) {
		delay = 6 * 60 * 1000;
	}

	// Minimum 10 seconds
	if (delay < 10000) {
		delay = 10000;
	}

	console.log("Next poll in " + Math.round(delay / 1000) + "s");
	pollTimer = setTimeout(fetchData, delay);
}

/**
 * Handle configuration page (Clay)
 */
Pebble.addEventListener("showConfiguration", function (e) {
	console.log("Showing configuration");
	// Pass current settings to Clay so the form shows saved values
	var claySettings = {
		accountName: settings.accountName,
		password: settings.password,
		server: settings.server,
		unit: settings.unit,
		reversed: settings.reversed,
		lowThreshold: settings.lowThreshold,
		highThreshold: settings.highThreshold,
		vibeLowSoonEnabled: settings.vibeLowSoonEnabled,
		vibeLowSoonThreshold: settings.vibeLowSoonThreshold,
		vibeLowSoonRepeatMinutes: settings.vibeLowSoonRepeatMinutes,
		vibeEnabled: settings.vibeEnabled,
		vibeHighThreshold: settings.vibeHighThreshold,
		vibeDelayMinutes: settings.vibeDelayMinutes,
		vibeRepeatMinutes: settings.vibeRepeatMinutes
	};

	Pebble.openURL(clay.generateUrl(claySettings));
});

/**
 * Handle configuration response (Clay)
 */
Pebble.addEventListener("webviewclosed", function (e) {
	console.log("Configuration closed");

	if (e && !e.response) {
		return;
	}

	var dict;

	try {
		dict = JSON.parse(e.response);
	} catch (e) {
		throw new Error("The provided response was not valid JSON");
	}

	// Update local settings from Clay response
	if (dict.accountName !== undefined) settings.accountName = dict.accountName.value || "";
	if (dict.password !== undefined) settings.password = dict.password.value || "";
	if (dict.server !== undefined) settings.server = dict.server.value || "us";
	if (dict.unit !== undefined) settings.unit = dict.unit.value || "mgdl";
	if (dict.reversed !== undefined) settings.reversed = !!dict.reversed.value;
	if (dict.highThreshold !== undefined) settings.highThreshold = parseInt(dict.highThreshold.value, 10) || 180;
	if (dict.lowThreshold !== undefined) settings.lowThreshold = parseInt(dict.lowThreshold.value, 10) || 70;
	if (dict.vibeLowSoonEnabled !== undefined) settings.vibeLowSoonEnabled = !!dict.vibeLowSoonEnabled.value;
	if (dict.vibeLowSoonThreshold !== undefined)
		settings.vibeLowSoonThreshold = parseInt(dict.vibeLowSoonThreshold.value, 10) || 80;
	if (dict.vibeLowSoonRepeatMinutes !== undefined)
		settings.vibeLowSoonRepeatMinutes = parseInt(dict.vibeLowSoonRepeatMinutes.value, 10) || 30;
	if (dict.vibeEnabled !== undefined) settings.vibeEnabled = !!dict.vibeEnabled.value;
	if (dict.vibeHighThreshold !== undefined)
		settings.vibeHighThreshold = parseInt(dict.vibeHighThreshold.value, 10) || 250;
	if (dict.vibeDelayMinutes !== undefined) settings.vibeDelayMinutes = parseInt(dict.vibeDelayMinutes.value, 10) || 60;
	if (dict.vibeRepeatMinutes !== undefined)
		settings.vibeRepeatMinutes = parseInt(dict.vibeRepeatMinutes.value, 10) || 60;

	saveSettings();

	// Reset session on credential change
	sessionId = null;

	// Fetch data with new settings
	fetchData();
});

/**
 * Handle ready event
 */
Pebble.addEventListener("ready", function () {
	console.log("T1000 PebbleKit JS ready");
	loadSettings();
	loadVibeState();
	fetchData();
});

/**
 * Handle app message from watch
 */
Pebble.addEventListener("appmessage", function (e) {
	console.log("Received message from watch");

	if (e.payload[KEY_REQUEST_DATA]) {
		console.log("Watch requested data update");
		fetchData();
	}
});
