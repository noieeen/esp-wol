// =========================================================================
// ESP32 Firebase Wake-on-LAN Controller
// =========================================================================
// Revision Date: 2025-04-24
// Author: Tharin Chaemchan
//
// Functionality:
// 1. Connects to WiFi.
// 2. Syncs time using NTP.
// 3. Connects to Firebase Realtime Database using Email/Password Auth.
// 4. Listens to a Firebase RTDB path (`FIREBASE_DIRECT_TRIGGER_PATH`) for a direct WOL trigger.
// 5. Periodically checks a Firebase RTDB queue (`FIREBASE_QUEUE_PATH`) for pending WOL tasks.
// 6. Sends WOL magic packets to specified MAC addresses via UDP broadcast.
// 7. Initializes Firebase structure if missing on startup.
//
// Requirements:
// - ESP32 Board
// - Arduino IDE with ESP32 Core installed (LATEST STABLE VERSION RECOMMENDED)
// - Firebase ESP Client Library by Mobizt (LATEST STABLE VERSION RECOMMENDED)
// - WakeOnLan Library by Rémi Sarrailh (Install via Library Manager)
// =========================================================================

#include <WiFi.h>
#include <Firebase_ESP_Client.h> // Install via Library Manager
#include <WakeOnLan.h>           // Install via Library Manager
#include <WiFiUdp.h>
#include <time.h>

// --- Configuration Section ---

// WiFi Credentials
const char* WIFI_SSID = "<ssid>";
const char* WIFI_PASSWORD = "<password>";

// Firebase Credentials & Configuration
#define FIREBASE_API_KEY "<api_key>" // Your Firebase Web API Key
#define FIREBASE_DATABASE_URL "<rdb_url>" // Your RTDB URL
#define FIREBASE_USER_EMAIL "<email>"           // Your Firebase Auth Email
#define FIREBASE_USER_PASSWORD "<password>"    // Your Firebase Auth Password

// Firebase Realtime Database Paths
const char* FIREBASE_BASE_PATH = "/wol"; // Base path for WOL data
const char* FIREBASE_DIRECT_TRIGGER_PATH = "/wol/proxmox"; // Path for the direct boolean trigger
const char* FIREBASE_QUEUE_PATH = "/wol/queue";      // Path for the WOL task queue

// Wake-on-LAN Configuration
const char* DIRECT_TRIGGER_TARGET_MAC = "00:00:00:00:00:00"; // MAC address for the direct trigger path

// NTP Configuration
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";
long        GMT_OFFSET_SEC = 7 * 3600; // GMT+7 for Thailand
int         DAYLIGHT_OFFSET_SEC = 0;   // No Daylight Saving Time

// Timing Intervals (milliseconds)
const unsigned long WIFI_CONNECT_TIMEOUT = 30000; // 30 seconds
const unsigned long NTP_SYNC_TIMEOUT = 30000;     // 30 seconds
const unsigned long FIREBASE_READY_TIMEOUT = 30000; // 30 seconds
const unsigned long QUEUE_CHECK_INTERVAL = 15000; // Check queue every 15 seconds
const unsigned long LOOP_DELAY = 200;             // Small delay in main loop for stability

// Options
const bool DELETE_COMPLETED_QUEUE_TASKS = false; // Set to true to delete tasks from queue after completion

// --- End Configuration Section ---

// Global Objects
FirebaseData fbdo_stream; // FirebaseData object for the stream connection
FirebaseData fbdo_general; // FirebaseData object for general operations (queue, init)
FirebaseAuth fb_auth;
FirebaseConfig fb_config;
WiFiUDP udp_client;
WakeOnLan wol_sender(udp_client);

// Timing variables
unsigned long lastQueueCheckMillis = 0;

// --- Function Prototypes ---
bool connectWiFi();
bool syncTimeNTP();
bool initFirebase();
bool checkAndInitFirebaseStructure();
void sendWOL(const char* macAddress);
void firebaseStreamCallback(FirebaseStream data);
void firebaseStreamTimeoutCallback(bool timeout);
void processWolQueue();

// --- Setup Function ---
void setup() {
    Serial.begin(115200);
    Serial.println("\n\n--- ESP32 Firebase WOL Controller ---");
    Serial.printf("Firmware Version: %s\n", __DATE__ " " __TIME__);

    // Disable WiFi Sleep (Important for stable connection)
    WiFi.setSleep(false);

    // 1. Connect to WiFi
    Serial.println("[SETUP] Connecting to WiFi...");
    if (!connectWiFi()) {
        Serial.println("[ERROR] WiFi connection failed! Halting execution.");
        while (1) { delay(1000); } // Halt
    }
    Serial.println("[SETUP] WiFi Connected.");
    Serial.print("  IP Address: ");
    Serial.println(WiFi.localIP());

    // 2. Synchronize Time
    Serial.println("[SETUP] Synchronizing time via NTP...");
    if (!syncTimeNTP()) {
        Serial.println("[WARN] Failed to sync time with NTP servers. SSL/TLS connections might fail.");
        // Continue execution but warn the user
    } else {
        Serial.println("[SETUP] Time Synchronized.");
    }

    // 3. Initialize Firebase Connection
    Serial.println("[SETUP] Initializing Firebase...");
    if (!initFirebase()) {
        Serial.println("[ERROR] Firebase initialization failed! Halting execution.");
        while (1) { delay(1000); } // Halt
    }
     Serial.println("[SETUP] Firebase Initialized and Ready.");

    // 4. Check and Initialize Firebase Database Structure
    Serial.println("[SETUP] Checking/Initializing Firebase RTDB structure...");
    if (!checkAndInitFirebaseStructure()) {
         Serial.println("[WARN] Failed to verify or initialize Firebase structure completely.");
         // Continue but warn
    } else {
         Serial.println("[SETUP] Firebase structure verified/initialized.");
    }

    // 5. Start Firebase Stream Listener for Direct Trigger
    Serial.printf("[SETUP] Starting Firebase stream on %s...\n", FIREBASE_DIRECT_TRIGGER_PATH);
    if (!Firebase.RTDB.beginStream(&fbdo_stream, FIREBASE_DIRECT_TRIGGER_PATH)) {
        Serial.printf("[ERROR] Failed to begin stream: %s\n", fbdo_stream.errorReason().c_str());
    } else {
        Firebase.RTDB.setStreamCallback(&fbdo_stream, firebaseStreamCallback, firebaseStreamTimeoutCallback);
        Serial.println("[SETUP] Stream listener started successfully.");
    }

    Serial.println("\n--- Setup Complete --- Entering Main Loop ---\n");
}

// --- Main Loop ---
void loop() {
    // Periodically check the WOL queue
    if (millis() - lastQueueCheckMillis >= QUEUE_CHECK_INTERVAL) {
        // Use the general FirebaseData object for the queue check
        processWolQueue();
        lastQueueCheckMillis = millis(); // Update last check time
    }

    // Short delay to yield CPU to background tasks (WiFi, Firebase, etc.)
    delay(LOOP_DELAY);
}

// --- WiFi Connection Function ---
bool connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("  Connecting");
    unsigned long startAttemptTime = millis();

    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < WIFI_CONNECT_TIMEOUT) {
        Serial.print(".");
        delay(500);
    }

    Serial.println(); // New line after dots
    return (WiFi.status() == WL_CONNECTED);
}

// --- NTP Time Synchronization Function ---
bool syncTimeNTP() {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
    Serial.print("  Waiting for time sync");
    time_t now = time(nullptr);
    unsigned long startAttemptTime = millis();

    // Wait until time is reasonably synced (year > 2023) or timeout
    while (now < 1704067200L && millis() - startAttemptTime < NTP_SYNC_TIMEOUT) { // 1704067200L is Jan 1, 2024 00:00:00 UTC
        delay(500);
        Serial.print(".");
        now = time(nullptr);
    }
    Serial.println();

    if (now >= 1704067200L) {
        struct tm timeinfo;
        getLocalTime(&timeinfo); // Use getLocalTime with configTime
        Serial.print("  Current time: ");
        Serial.print(asctime(&timeinfo)); // asctime adds a newline
        return true;
    } else {
        return false;
    }
}

// --- Firebase Initialization Function ---
bool initFirebase() {
    fb_config.api_key = FIREBASE_API_KEY;
    fb_config.database_url = FIREBASE_DATABASE_URL;
    fb_auth.user.email = FIREBASE_USER_EMAIL;
    fb_auth.user.password = FIREBASE_USER_PASSWORD;

    // Assign the callback function for token status events (optional but helpful)
    fb_config.token_status_callback = [](TokenInfo info) {
        Serial.printf("  Token Status: %s (%s)\n",
                      info.status == token_status_ready ? "Ready" : "Error/Waiting",
                      info.status == token_status_error ? info.error.message.c_str() : "...");
    };

    // Initialize Firebase library
    Firebase.begin(&fb_config, &fb_auth);
    Firebase.reconnectWiFi(true); // Allow library to handle WiFi reconnections

    // Optional: Set higher buffer sizes if experiencing data truncation issues (uncomment if needed)
    // fbdo_stream.setBSSLBufferSize(2048, 2048); // Rx, Tx for stream
    // fbdo_general.setBSSLBufferSize(1024, 1024); // Rx, Tx for general
    // fbdo_stream.setResponseSize(1024);
    // fbdo_general.setResponseSize(1024);

    // Wait for Firebase to be ready (authenticated)
    Serial.print("  Waiting for Firebase Authentication");
    unsigned long startWaitTime = millis();
    while (!Firebase.ready() && millis() - startWaitTime < FIREBASE_READY_TIMEOUT) {
        Serial.print(".");
        delay(300);
    }
    Serial.println();

    return Firebase.ready();
}

// --- Firebase Structure Initialization Function ---
bool checkAndInitFirebaseStructure() {
    bool success = true;

    // Check/Init Direct Trigger Path
    Serial.printf("  Checking path: %s\n", FIREBASE_DIRECT_TRIGGER_PATH);
    if (!Firebase.RTDB.get(&fbdo_general, FIREBASE_DIRECT_TRIGGER_PATH)) {
        if (fbdo_general.httpCode() == FIREBASE_ERROR_PATH_NOT_EXIST || fbdo_general.dataType() == "null") {
            Serial.printf("    Path '%s' missing or null, initializing to 'false'.\n", FIREBASE_DIRECT_TRIGGER_PATH);
            if (!Firebase.RTDB.setBool(&fbdo_general, FIREBASE_DIRECT_TRIGGER_PATH, false)) {
                Serial.printf("[ERROR] Failed to initialize '%s': %s\n", FIREBASE_DIRECT_TRIGGER_PATH, fbdo_general.errorReason().c_str());
                success = false;
            }
        } else {
            Serial.printf("[WARN] Error checking '%s': %s\n", FIREBASE_DIRECT_TRIGGER_PATH, fbdo_general.errorReason().c_str());
            // Consider it non-fatal for now
        }
    } else {
        Serial.printf("    Path '%s' exists.\n", FIREBASE_DIRECT_TRIGGER_PATH);
    }

    // Check/Init Queue Path
    Serial.printf("  Checking path: %s\n", FIREBASE_QUEUE_PATH);
    if (!Firebase.RTDB.get(&fbdo_general, FIREBASE_QUEUE_PATH)) {
        if (fbdo_general.httpCode() == FIREBASE_ERROR_PATH_NOT_EXIST || fbdo_general.dataType() == "null") {
            Serial.printf("    Path '%s' missing or null, initializing to empty object {}.\n", FIREBASE_QUEUE_PATH);
            FirebaseJson emptyJson;
            emptyJson.setJsonData("{}");
            if (!Firebase.RTDB.setJSON(&fbdo_general, FIREBASE_QUEUE_PATH, &emptyJson)) {
                Serial.printf("[ERROR] Failed to initialize '%s': %s\n", FIREBASE_QUEUE_PATH, fbdo_general.errorReason().c_str());
                success = false;
            }
        } else {
            Serial.printf("[WARN] Error checking '%s': %s\n", FIREBASE_QUEUE_PATH, fbdo_general.errorReason().c_str());
            // Consider it non-fatal for now
        }
    } else {
         Serial.printf("    Path '%s' exists.\n", FIREBASE_QUEUE_PATH);
    }

    return success;
}


// --- WOL Sending Function ---
void sendWOL(const char* macAddress) {
    if (!macAddress || strlen(macAddress) != 17) {
        Serial.printf("[ERROR] Invalid MAC address format for WOL: %s\n", macAddress ? macAddress : "NULL");
        return;
    }

    IPAddress localIP = WiFi.localIP();
    IPAddress broadcastIP = localIP;
    broadcastIP[3] = 255; // Calculate broadcast address for the local subnet

    Serial.printf("[INFO] Sending WOL Magic Packet to MAC: %s via Broadcast IP: %s\n",
                  macAddress, broadcastIP.toString().c_str());

    // Send the packet (usually 3-5 times for reliability)
    for (int i=0; i < 3; ++i) {
        wol_sender.sendMagicPacket(macAddress, broadcastIP);
        delay(50); // Small delay between packets
    }
}

// --- Firebase Stream Callback (Direct Trigger) ---
void firebaseStreamCallback(FirebaseStream data) {
    // FIX: Removed data.getEvent() as it's not available in all library versions
    Serial.printf("[STREAM] Path: %s, Type: %s, Value: %s\n",
                  data.dataPath().c_str(),
                  data.dataType().c_str(),
                  data.stringData().c_str());

    // Check if it's boolean data and the value is true
    if (data.dataTypeEnum() == fb_esp_rtdb_data_type_boolean && data.boolData()) {
        Serial.printf("[INFO] Direct WOL Trigger received for path: %s\n", FIREBASE_DIRECT_TRIGGER_PATH);

        sendWOL(DIRECT_TRIGGER_TARGET_MAC); // Send WOL to the predefined MAC

        // Reset the trigger flag back to false in Firebase
        Serial.printf("  Resetting trigger flag '%s' to false...\n", FIREBASE_DIRECT_TRIGGER_PATH);
        // Use the general FirebaseData object to avoid conflicts with the stream object
        if (!Firebase.RTDB.setBool(&fbdo_general, FIREBASE_DIRECT_TRIGGER_PATH, false)) {
            Serial.printf("[ERROR] Failed to reset trigger flag: %s\n", fbdo_general.errorReason().c_str());
        } else {
            Serial.println("  Trigger flag reset successfully.");
        }
    }
}

// --- Firebase Stream Timeout Callback ---
void firebaseStreamTimeoutCallback(bool timeout) {
    if (timeout) {
        Serial.println("[WARN] Firebase stream timeout occurred. Reconnecting...");
        // The library might handle reconnection automatically if configured.
        // Explicitly ending and beginning can sometimes help if auto-reconnect fails.
        // Firebase.RTDB.endStream(&fbdo_stream); // Optional: Explicitly end
        // delay(1000);                          // Optional: Delay before restart
        // if (!Firebase.RTDB.beginStream(&fbdo_stream, FIREBASE_DIRECT_TRIGGER_PATH)) {
        //     Serial.printf("[ERROR] Failed to restart stream after timeout: %s\n", fbdo_stream.errorReason().c_str());
        // }
    }
}

// --- Firebase WOL Queue Processing Function ---
void processWolQueue() {
    if (!Firebase.ready()) {
        Serial.println("[QUEUE] Firebase not ready, skipping queue check.");
        return;
    }

    Serial.printf("[QUEUE] Checking WOL queue at %s...\n", FIREBASE_QUEUE_PATH);

    // Use the general FirebaseData object (fbdo_general)
    if (Firebase.RTDB.getJSON(&fbdo_general, FIREBASE_QUEUE_PATH)) {
        FirebaseJson *json = fbdo_general.jsonObjectPtr(); // Get pointer for iteration

        size_t taskCount = json->iteratorBegin();
        Serial.printf("  Found %d potential tasks.\n", taskCount);

        if (taskCount == 0) {
            json->iteratorEnd(); // End iteration even if empty
            return; // Nothing to process
        }

        String key, value;
        int type;
        std::vector<String> completedTaskIds; // Store IDs of tasks to delete if option enabled

        for (size_t i = 0; i < taskCount; i++) {
            json->iteratorGet(i, type, key, value); // Get task ID (key)

            if (type == FirebaseJson::JSON_OBJECT) {
                String taskId = key;
                String taskPath = String(FIREBASE_QUEUE_PATH) + "/" + taskId;
                FirebaseJsonData taskData; // Helper object to extract fields

                Serial.printf("  Inspecting Task ID: %s (Path: %s)\n", taskId.c_str(), taskPath.c_str());

                // FIX: Fetch the specific task object first into fbdo_general
                if (Firebase.RTDB.get(&fbdo_general, taskPath)) {

                    // Check if the fetched data is a JSON object
                    if (fbdo_general.dataTypeEnum() == fb_esp_rtdb_data_type_json) {

                        // FIX: Extract fields from the result in fbdo_general.jsonObject()
                        if (fbdo_general.jsonObject().get(taskData, "status")) {
                            if (taskData.success && taskData.stringValue == "pending") {
                                Serial.printf("    Task '%s' has status 'pending'. Processing...\n", taskId.c_str());

                                String macAddress = "";
                                int delayMs = 0;

                                // Extract MAC address from the fetched task object
                                if (fbdo_general.jsonObject().get(taskData, "mac") && taskData.success) {
                                    macAddress = taskData.stringValue;
                                } else {
                                    Serial.printf("[WARN] Task '%s': Failed to extract 'mac' field.\n", taskId.c_str());
                                    Firebase.RTDB.setString(&fbdo_general, taskPath + "/status", "error_missing_mac");
                                    continue; // Continue outer loop (skip to next task)
                                }

                                // Extract Delay (optional) from the fetched task object
                                if (fbdo_general.jsonObject().get(taskData, "delay") && taskData.success) {
                                    if (taskData.typeNum == FirebaseJson::JSON_INT || taskData.typeNum == FirebaseJson::JSON_FLOAT) {
                                        delayMs = taskData.intValue;
                                    } else {
                                        delayMs = taskData.stringValue.toInt(); // Try converting if stored as string
                                    }
                                    if (delayMs < 0) delayMs = 0; // Ensure delay isn't negative
                                }
                                // If delay field is missing, delayMs remains 0 (no delay)

                                Serial.printf("      MAC: %s, Delay: %d ms\n", macAddress.c_str(), delayMs);

                                // Apply delay if specified
                                if (delayMs > 0) {
                                    Serial.printf("      Applying delay of %d ms...\n", delayMs);
                                    delay(delayMs);
                                }

                                // Send the WOL packet
                                sendWOL(macAddress.c_str());

                                // Update status to "done"
                                Serial.printf("      Updating status to 'done'...\n"); // Removed taskId from printf as it's clear from context
                                if (Firebase.RTDB.setString(&fbdo_general, taskPath + "/status", "done")) {
                                    Serial.printf("    Task '%s' completed successfully.\n", taskId.c_str());
                                    if (DELETE_COMPLETED_QUEUE_TASKS) {
                                        completedTaskIds.push_back(taskId); // Mark for deletion
                                    }
                                } else {
                                    Serial.printf("[ERROR] Task '%s': Failed to update status to 'done': %s\n", taskId.c_str(), fbdo_general.errorReason().c_str());
                                }

                            } else if (!taskData.success) {
                                Serial.printf("[WARN] Task '%s': Could not read 'status' field.\n", taskId.c_str());
                            } else {
                                // Status is not pending, ignore for processing
                                Serial.printf("    Task '%s': Status is '%s' (not 'pending'). Skipping.\n", taskId.c_str(), taskData.stringValue.c_str());
                                // Optionally check if status is already "done" and mark for deletion if needed
                                if (DELETE_COMPLETED_QUEUE_TASKS && taskData.stringValue == "done") {
                                    completedTaskIds.push_back(taskId);
                                }
                            }
                        } else { // End if get status field succeeded
                             Serial.printf("[WARN] Task '%s': Could not get 'status' field data from fetched object.\n", taskId.c_str());
                        }
                        // taskData.clear(); // No need to clear, it's reused by .get()

                    } else { // End if fetched data is JSON
                        Serial.printf("[WARN] Fetched data for task '%s' is not JSON (Type: %s). Skipping.\n", taskId.c_str(), fbdo_general.dataType().c_str());
                    }
                } else { // End if get task object succeeded
                    Serial.printf("[ERROR] Failed to fetch task object for Task ID %s: %s\n", taskId.c_str(), fbdo_general.errorReason().c_str());
                }

            } else { // End if type == JSON_OBJECT
                Serial.printf("[WARN] Skipping item '%s' at index %d in queue - it's not a JSON object.\n", key.c_str(), i);
            }
        } // End for loop iterating through tasks

        json->iteratorEnd(); // Release iterator resources

        // Delete completed tasks if option is enabled
        if (DELETE_COMPLETED_QUEUE_TASKS && !completedTaskIds.empty()) {
             Serial.printf("[QUEUE] Deleting %d completed tasks...\n", completedTaskIds.size());
             for (const String& taskIdToDelete : completedTaskIds) {
                 String taskPathToDelete = String(FIREBASE_QUEUE_PATH) + "/" + taskIdToDelete;
                 Serial.printf("  Deleting node: %s\n", taskPathToDelete.c_str());
                 // IMPORTANT: Use a *different* FirebaseData object for deletion OR ensure fbdo_general is reset if deletion fails mid-way
                 // For simplicity here, we continue using fbdo_general, but be aware if many deletions fail, its state might be affected.
                 if (!Firebase.RTDB.deleteNode(&fbdo_general, taskPathToDelete)) {
                     Serial.printf("[ERROR] Failed to delete task '%s': %s\n", taskIdToDelete.c_str(), fbdo_general.errorReason().c_str());
                 }
             }
        }

    } else { // End if getJSON succeeded for the whole queue
        // Handle getJSON failure - distinguish between empty queue and actual error
        if (fbdo_general.httpCode() == 200 && fbdo_general.dataType() == "null") {
             Serial.println("  Queue is empty or does not exist (treated as empty).");
        } else {
            Serial.printf("[ERROR] Failed to fetch queue data from '%s': %s\n", FIREBASE_QUEUE_PATH, fbdo_general.errorReason().c_str());
        }
    }
     Serial.println("[QUEUE] Queue check finished.");
}
