#include <BLE-MIDI\src\BLEMIDI_Transport.h>
#include <hardware/BLEMIDI_ESP32.h>
#include <FastLED>

BLEMIDI_CREATE_INSTANCE("Gabriel's Foot Controller", MIDI);

#define DATA_PIN    19       // Pin where the LED strip is connected
#define NUM_LEDS    10       // Number of LEDs in the strip
#define LED_TYPE    WS2812B  // LED strip type (e.g., WS2812B, WS2811, etc.)
#define COLOR_ORDER GRB      // Color order (depends on your strip)

CRGB leds[NUM_LEDS];         // Array holding LED data

#ifndef LED_BUILTIN
#define LED_BUILTIN 2        // Built-in LED pin definition (adjust for your board)
#endif

const int num_buttons = 10;
const int button_pins[num_buttons] = {13, 14, 25, 32, 17, 12, 21, 33, 4, 27};

int current_button_state[num_buttons] = {};
int previous_button_state[num_buttons] = {};
unsigned long last_press_time[num_buttons] = {0};  // Last press time for each button
bool long_press_sent[num_buttons] = {false};       // Indicates if the long press note has been sent

unsigned long last_debounce_time[num_buttons] = {0};  // Last time the button state was toggled
unsigned long debounce_delay = 50;                    // Debounce delay in ms

byte midi_channel = 1;
byte base_note = 34;

unsigned long last_blink_time = 0;  // Last time the LED blinked
bool led_state = false;             // Tracks if the LED is ON or OFF
bool is_connected = false;

int bpm_msb = 0;              // Most Significant Byte for BPM
int bpm_lsb = 0;              // Least Significant Byte for BPM
int bpm = 60;                 // Full BPM value after combining MSB and LSB

std::map<int, CRGB> noteColorMap = {
    {0, CRGB(255, 0, 180)},  // Pink
    {1, CRGB(73, 73, 255)},      // Blue
    {2, CRGB(63, 210, 63)},      // Green
    {3, CRGB(200, 200, 56)},    // Yellow
    {6, CRGB(68, 256, 68)},  // Light Green
    {5, CRGB(0, 256, 179)},  // Light Blue
    {4,CRGB(91, 119, 256)},           // White
    // {41, CRGB::White},          // White
    // {42, CRGB::White},          // White
    // {43, CRGB::White}           // White
};

// Function declarations for multitasking (FreeRTOS)
void readMIDI(void *parameter);  // Task for reading MIDI events (Core 1)
void handleLEDs(void *parameter); // Task for LED control (Core 0)

void setup() {
    Serial.begin(115200);
    MIDI.begin(MIDI_CHANNEL_OMNI);

    // FastLED initialization
    FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
    FastLED.setBrightness(128); // Set LED brightness (0-255)

    // BLEMIDI connection event handlers
    BLEMIDI.setHandleConnected([]() {
        Serial.println("---------CONNECTED---------");
        is_connected = true;
        digitalWrite(LED_BUILTIN, HIGH);
    });

    BLEMIDI.setHandleDisconnected([]() {
        Serial.println("---------DISCONNECTED---------");
        is_connected = false;
        digitalWrite(LED_BUILTIN, LOW);
    });

    // Handle incoming MIDI control changes for BPM updates
    MIDI.setHandleControlChange([](byte channel, byte number, byte value) {
        if (number == 123) {
            bpm_msb = value;
        }
        if (number == 124) {
            bpm_lsb = value;
        }
        bpm = (bpm_msb << 7) | bpm_lsb;  // Combine MSB and LSB to get full BPM
        Serial.print("Calculated BPM: ");
        Serial.println(bpm);
    });

    // Handle MIDI note on event
    MIDI.setHandleNoteOn([](byte channel, byte note, byte velocity) {
        handleNoteOn(note);
        Serial.println(note);
    });

    // Handle MIDI note off event
    MIDI.setHandleNoteOff([](byte channel, byte note, byte velocity) {
        handleNoteOff(note);
        Serial.println(note);

    });

    // Create tasks for MIDI reading (Core 1) and LED handling (Core 0)
    xTaskCreatePinnedToCore(readMIDI, "MIDI-READ", 3000, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(handleLEDs, "LED-TASK", 3000, NULL, 1, NULL, 0);

    // Set Bluetooth Low Energy transmission power level
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);

    // Initialize built-in LED pin
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    // Initialize all button pins
    for (int i = 0; i < num_buttons; i++) {
        pinMode(button_pins[i], INPUT_PULLUP);
    }
}

void loop() {
    handleButtons();  // Manage button inputs in the main loop
}

// Button handling logic with debouncing and long press detection
void handleButtons() {
    for (int i = 0; i < num_buttons; i++) {
        int current_read = digitalRead(button_pins[i]);

        // Handle button state changes with debounce
        if (current_read != previous_button_state[i]) {
            last_debounce_time[i] = millis();  // Reset debounce timer
        }

        if ((millis() - last_debounce_time[i]) > debounce_delay) {
            if (current_read != current_button_state[i]) {
                current_button_state[i] = current_read;

                // Button pressed (LOW)
                if (current_button_state[i] == LOW) {
                    last_press_time[i] = millis();
                    long_press_sent[i] = false;

                // Button released (HIGH)
                } else {
                    if (!long_press_sent[i]) {
                        MIDI.sendNoteOn(base_note + i, 127, midi_channel);
                    }
                }
            }

            // Check for long press event (button held for > 2000 ms)
            if (current_button_state[i] == LOW) {
                unsigned long press_duration = millis() - last_press_time[i];
                if (press_duration > 2000 && !long_press_sent[i]) {
                    MIDI.sendNoteOn(base_note + i, 127, midi_channel + 1);
                    long_press_sent[i] = true;
                }
            }
        }

        previous_button_state[i] = current_read;  // Update previous state
    }
}

// Task to handle LED blinking based on BPM (runs on Core 0)
void handleLEDs(void *parameter) {
    Serial.print("LED Task running on core: ");
    Serial.println(xPortGetCoreID());
    
    for (;;) {
        blinkLEDByBPM();
        vTaskDelay(10 / portTICK_PERIOD_MS);  // Adjust as necessary
    }
}

// LED blink control based on current BPM
void blinkLEDByBPM() {
    if (bpm > 0) {
        unsigned long interval = (60000 / bpm) / 2;

        if (millis() - last_blink_time >= interval) {
            last_blink_time = millis();
            led_state = !led_state;

            leds[7] = led_state ? CRGB::White : CRGB::Black;  // Toggle LED state
            FastLED.show();
        }
    }
}

// Task for continuous MIDI reading (runs on Core 1)
void readMIDI(void *parameter) {
    Serial.print("MIDI Task started on core: ");
    Serial.println(xPortGetCoreID());
    
    for (;;) {
        MIDI.read();  // Continuously read incoming MIDI messages
        vTaskDelay(1 / portTICK_PERIOD_MS);  // Small delay to allow task switching
    }
}

// Handles turning on the LED for the corresponding MIDI note
void handleNoteOn(int index) {
        leds[index] = noteColorMap[index];
        FastLED.show();
    
}

// Handles turning off the LED for the corresponding MIDI note
void handleNoteOff(int index) {
    leds[index] = CRGB(0, 0, 0);  // Turn off the LED
    FastLED.show();
}
