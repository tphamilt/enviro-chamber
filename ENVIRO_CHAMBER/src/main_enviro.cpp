/** @file main_web.cpp
 *    This file contains code to get a really simple-minded web server going as
 *    a demonstration of the ESP32's ability to talk through the Interwebs. 
 *    The stuff that really matters is in @c task_wifi.*
 * 
 *  @author JR Ridgely
 *  @date   2020-Nov-02 
 */

#include <Arduino.h>
#include <string>
#include <PrintStream.h>
#include <Adafruit_MAX31856.h>
#include <WiFi.h>
#include <SPI.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <Wire.h>
//#include "task_wifi.h"
#include "taskshare.h"

/// DRDY PIN FOR THERM
#define DRDY_PIN 25
/// CHIP SELECT 1 PIN
#define CS1_PIN 4     
/// CHIP SELECT 2 PIN
#define CS2_PIN 5   
/// CHIP SELECT 3 PIN
#define CS3_PIN 6   
/// HEATER CONTROL PIN
#define HEATER_PIN 27 
/// SCK PIN NUMBER
#define SCK 30       
/// SDO PIN NUMBER 
#define SDO 31     
/// SDI PIN NUMBER   
#define SDI 37       
/// THRESHOLD FOR HEATER ON/OFF
#define THRESHOLD 10

/// Share to communicate the desired temperature setpoint
Share<int16_t> desired_temp ("Temperature");

/// Share to communicate the current temperature reading
Share<int16_t> temp_reading ("Curr Temp");

/// A pointer to the web server object
AsyncWebServer* p_server = NULL;

/// String for the input parameter
const char* PARAM_INT = "inputInt";

// HTML web page to handle input field (inputInt)
const char index_html[] PROGMEM = R"rawliteral(
    <!DOCTYPE HTML><html><head>
    <title>ESP Input Form</title>
    <h1>Enviro Chamber Test</h1>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <script>
        function submitMessage() {
        alert("Saved value to ESP SPIFFS");
        setTimeout(function(){ document.location.reload(false); }, 500);   
        }
    </script></head><body>
    <form action="/get" target="hidden-form">
        Setpoint Temperature (in &degC): 
        <input type="number " name="inputInt">
        <input type="submit" value="Submit" onclick="submitMessage()">
    </form><br>
    <iframe style="display:none" name="hidden-form"></iframe>
    </body></html>)rawliteral";

/** @brief   Read a character array from a serial device, echoing input.
 *  @details This function reads characters which are typed by a user into a
 *           serial device. It uses the Arduino function @c readBytes(), which
 *           blocks the task which calls this function until a character is
 *           read. When any character is received, it is echoed through the
 *           serial port so the user can see what was typed. 
 * 
 *           @b NOTE: When run on an ESP32 using FreeRTOS, this function must
 *           allow other tasks to run, so that the watchdog timer is reset.
 *           Otherwise the CPU will be reset.  The @c vTaskDelay() call does
 *           this. 
 *  @param   stream The serial device such as @c Serial used to communicate
 *  @param   buffer A character buffer in which to store the string; this
 *           buffer had better be at least @c size characters in length
 *  @param   size At most (this many - 1) characters will be read and stored 
 */
void enterStringWithEcho (Stream& stream, char* buffer, uint8_t size)
{
    int ch_in = 0;                            // One character from the buffer
    uint8_t count = 0;                        // Counts characters received

    // Read until return is received or too many characters have been read.
    // The readBytes function blocks while waiting for characters
    while (true)
    {
        ch_in = stream.read ();               // Gets char. or -1 if none there
        if (ch_in > 0)
        {
            stream.print ((char)ch_in);       // Echo the character
            if (ch_in == '\b' && count)       // If a backspace, back up one
            {                                 // character and try again
                count -= 2;
            }
            else if (ch_in == '\r')           // Ignore carriage returns
            {
            }
            else if (ch_in == '\n' || count >= (size - 1))
            {
                buffer[count] = '\0';         // String must have a \0 at end
                return;
            }
            else
            {
                buffer[count++] = ch_in;
            }
        }
        else
        {
            // If using FreeRTOS, yield so other tasks can run
            #ifdef FREERTOS_CONFIG_H
                vTaskDelay (1);
            #endif
        }
    }
}

/** @brief   Handle not found error.
 *  @details This function handles a notfound error for the wifi server
 * 
 *           NOTE: This function is included from 
 *           https://randomnerdtutorials.com/esp32-esp8266-input-data-html-form/
 *  @param   size At most (this many - 1) characters will be read and stored 
 */
void notFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Not found");
}

/** @brief   Read user input from file.
 *  @details This function reads the user input from a file.
 *           The file is opened and then checked for any errors before attempting
 *           to read the user input.
 * 
 *           @b NOTE: This function is included from 
 *           https://randomnerdtutorials.com/esp32-esp8266-input-data-html-form/
 *  @param   fs SPIFFS adress file
 *  @param   path the path to look for the file
 * 
 *  @return  returns the string read from the file
 */
String readFile(fs::FS &fs, const char * path){
  //Serial.printf("Reading file: %s\r\n", path);
  File file = fs.open(path, "r");
  //do you have to close files in c++? - Trent
  if(!file || file.isDirectory()){
    Serial.println("- empty file or failed to open file");
    return String();
  }
  //Serial.println("- read from file:");
  String fileContent;
  while(file.available()){
    fileContent+=String((char)file.read());
  }
  //Serial.println(fileContent);
  return fileContent;
}

/** @brief   Write user input to file.
 *  @details This function writes the user input to a file.
 *           The file is opened and then checked for any errors before attempting
 *           to write the user input.
 * 
 *           @b NOTE: This function is included from 
 *           https://randomnerdtutorials.com/esp32-esp8266-input-data-html-form/
 *  @param   fs SPIFFS adress file
 *  @param   path the path to look for the file
 *  @param   message what to write to the file
 */
void writeFile(fs::FS &fs, const char * path, const char * message){
  //Serial.printf("Writing file: %s\r\n", path);
  File file = fs.open(path, "w");
  if(!file){
    Serial.println("- failed to open file for writing");
    return;
  }
  if(file.print(message)){
    //Serial.println("- file written");
  } else {
    Serial.println("- write failed");
  }
}

// Replaces placeholder with stored values
String processor(const String& var){
  //Serial.println(var);
  if(var == "inputInt"){
    return readFile(SPIFFS, "/inputInt.txt");
  }
  return String();
}

/** @brief   Task which controls the WiFi module to run a web server.
 *  @param   p_params Pointer to parameters, which is not used
 * 
 * code for input data inspired by 
 * https://randomnerdtutorials.com/esp32-esp8266-input-data-html-form/
 */
void task_WiFi (void* p_params)
{
    (void)p_params;

    // Create a web server object which will listen on TCP port 80
    AsyncWebServer server (80);
    p_server = &server;

    // Enter the password for your WiFi network
    char essid_buf[36];
    char pw_buf[36];
    Serial << "Enter WiFi SSID: ";
    enterStringWithEcho (Serial, essid_buf, 34);
    Serial << "Enter WiFi password: ";
    enterStringWithEcho (Serial, pw_buf, 34);

    // Connect to your WiFi network
    Serial << endl << "WiFi connecting to \"" << essid_buf << "\""
           << " with password \"" << pw_buf << "\"";
    WiFi.begin (essid_buf, pw_buf);
    WiFi.setHostname ("ESP32 Weather");

    // Take whatever time is necessary to connect to the WiFi network
    while (WiFi.status() != WL_CONNECTED) 
    {
        delay (1000);
        Serial << ".";
    }
    Serial << endl << "WiFi connected at IP " << WiFi.localIP () << endl;

    // Send web page with input fields to client
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
        });

    // Send a GET request to <ESP_IP>/get?input1=<inputMessage>
    server.on("/get", HTTP_GET, [] (AsyncWebServerRequest *request) {
    String inputMessage;
    String inputParam;
    
    // GET inputInt value on <ESP_IP>/get?inputInt=<inputMessage>
    if (request->hasParam(PARAM_INT)) {
      inputMessage = request->getParam(PARAM_INT)->value();
      writeFile(SPIFFS, "/inputInt.txt", inputMessage.c_str());
    }
    else {
      inputMessage = "No message sent";
    }
    Serial.println(inputMessage);
    request->send(200, "text/html", "HTTP GET request sent to your ESP on input field (" 
                                     + inputParam + ") with value: " + inputMessage +
                                     "<br><a href=\"/\">Return to Home Page</a>");
    });


    // Install callback functions to handle web requests
    //server.on ("/", handle_OnConnect);
    server.onNotFound (notFound);

    // Get the web server up and running
    server.begin ();
    Serial << "HTTP server started." << endl;

    // This has been moved from loop(), where it lived in example code
    for (;;)
    {
        //server.handleClient ();
        // To access your stored values on inputInt
        int yourInputInt = readFile(SPIFFS, "/inputInt.txt").toInt();
        Serial.print("*** Your inputInt: ");
        Serial.println(yourInputInt);
        //desired_temp.put(yourInputInt); --> uses too much stack space?
        vTaskDelay (5000);
    }
}

/** @brief   Task which controls the heating element
 *  @param   p_params Pointer to parameters, which is not used
 * 
 */
void task_heater(void* p_params){
    (void)p_params;

    int16_t setpoint = 20;
    int16_t current = 0;

    //set pin mode to output for heater control
    pinMode(HEATER_PIN, OUTPUT);

    for(;;){
        desired_temp.get(setpoint);
        temp_reading.get(current);
        if(current < (setpoint - THRESHOLD)){
            digitalWrite(HEATER_PIN, 1);
        }
        else{
            digitalWrite(HEATER_PIN, 0);
        }
        vTaskDelay(100);
    }
}

/** @brief   Task which reads data from thermocouples.
 *  @param   p_params Pointer to parameters, which is not used
 * 
 */
void task_sensor(void* p_params){
    (void)p_params;

    Adafruit_MAX31856 therm1 = Adafruit_MAX31856(CS1_PIN, SDI, SDO, SCK);
    pinMode(DRDY_PIN, INPUT);

    if (!therm1.begin()) {
        Serial.println("Could not initialize thermocouple.");
        while (1) delay(10);
    }

    therm1.setThermocoupleType(MAX31856_TCTYPE_T);
    therm1.setConversionMode(MAX31856_CONTINUOUS);

    for(;;){
        int count = 0;
        while (digitalRead(DRDY_PIN)) {
            if (count++ > 200) {
                count = 0;
                Serial.print(".");
            }
        }
        Serial.println(therm1.readThermocoupleTemperature());
        vTaskDelay (500);
    }
}

/** @brief   Set up the ESP32
 *  @details This program runs the tasks to control the heater
 *           and run the web server.
 */
void setup (void) 
{
    if(!SPIFFS.begin(true)){
      Serial.println("An Error has occurred while mounting SPIFFS");
      return;
    }

    Serial.begin (115200);
    delay (1000);

    // Create a task to run the WiFi connection. This task needs a lot of stack
    // space to prevent it crashing
    
    xTaskCreate (task_WiFi,
                 "WiFi",
                 4500,
                 NULL,
                 1,
                 NULL);
                 
    xTaskCreate (task_sensor,
                "sensor",
                1000,
                NULL,
                1,
                NULL);

    xTaskCreate (task_heater,
                "heater",
                1000,
                NULL,
                3,
                NULL);
}


/** @brief   The Arduino loop function, which is unused in this program.
 */
void loop (void)
{
}


