#include "DuckyPayload.h"

#include <DuckyParse.h>
#include <uptime.h>

#include "../../Debug/Logging.h"

#include "../../Devices/Button/HardwareButton.h"
#include "../../Devices/Storage/HardwareStorage.h"
#include "../../Devices/USB/USBCore.h"
#include "../../Devices/USB/USBHID.h"
#include "../../Devices/USB/USBMSC.h"
#include "../../Devices/USB/USBNCM.h"
#include "../../Devices/TFT/HardwareTFT.h"
#include "../../Devices/LED/HardwareLED.h"
#include "../../Devices/WiFi/HardwareWiFi.h"

#include "../../Attacks/Marauder/Marauder.h"
#include "../../Attacks/Agent/Agent.h"

#ifdef DUCKY_CUSTOM_LOG
#warning Enabling DuckyParse logging increases build size
extern void DuckyInterpreterLog(uint8_t level, const char *format, ...)
{
    if (level >= 2) // Warning
    {
        // Define a buffer size
        const int bufferSize = 1024;
        char buffer[bufferSize];

        // Initialize the variable argument list
        va_list args;
        va_start(args, format);

        // Format the string into the buffer
        vsnprintf(buffer, bufferSize, format, args);

        // End the variable argument list
        va_end(args);

        // Create a std::string from the buffer
        std::string logMessage(buffer);
        Debug::Log.info("DUCKY", logMessage);
    }
}
#endif

static std::string currentlyExecutingFile;
static uint8_t totalErrors = 0;
static int lastExecutionResult = 0;
static volatile uint32_t timeToWait = 0; // volatile to try and prevent dirty reads
static bool firstRun = true;
static bool requiresReset = false;
static std::unordered_map<std::string, std::function<int(std::string, std::unordered_map<std::string, std::string>, std::unordered_map<std::string, int>)>> extCommands;
static std::vector<std::function<std::pair<std::string, std::string>()>> consts;
static std::string localCmdLineToExecute;
static bool wasLastPressLong = false; // For buttons

static std::string readCmdLine(const std::string &filename, int lineNum);

namespace Attacks
{
    DuckyPayload Ducky;
}

static void reset()
{
    // to ensure we have flushed all keystrokes etc we do this on the next loop
    requiresReset = true;
}

static std::string readLineFromFileOrCmdLine(const std::string &filename, const int lineNumber)
{
    if (filename.length() == 0)
    {
        // we are reading the a given cmdline
        Debug::Log.info(LOG_DUCKY, "Reading cmd line " + std::to_string(lineNumber));

        auto temp = std::string(localCmdLineToExecute);
        localCmdLineToExecute.clear();

        return temp;
    }
    else
    {
        // we are reading a file
        return Devices::Storage.readLineFromFile(filename, lineNumber);
    }
}

static void keyboard_press(const uint8_t modifiers, const uint8_t key1, const uint8_t key2, const uint8_t key3, const uint8_t key4, const uint8_t key5, const uint8_t key6)
{
    Devices::USB::HID.keyboard_press(modifiers, key1, key2, key3, key4, key5, key6);
}

static void keyboard_release()
{
    Devices::USB::HID.keyboard_release();
}

static void changeLEDState(bool on, uint8_t hue, uint8_t saturation, uint8_t lum, uint8_t brightness)
{
    Devices::LED.changeLEDState(on, hue, saturation, lum, brightness);
}

void ButtonWaitTask(void *arg)
{
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(150));
        if (Devices::Button.hasButtonBeenPressed())
        {
            wasLastPressLong = Devices::Button.wasLastPressLong();
            Devices::Button.resetButtonPressedState();
            break;
        }
    }

    timeToWait = 0;
    vTaskDelete(NULL);
}

static void waitForButton()
{
    // We can do other things while we are waiting so we kick off a task to wait
    // return, which causes us to loop again
    // in our loop we check if the task is finished and only continue processing if it has
    timeToWait = -1;

    xTaskCreate(
        ButtonWaitTask, // Function that should be called
        "ButtonWait",   // Name of the task (for debugging)
        1000,           // Stack size (bytes)
        NULL,           // Parameter to pass
        1,              // Task priority
        NULL            // Task handle
    );
}

static void changeUSBMode(DuckyInterpreter::USB_MODE &mode, const uint16_t &vid, const uint16_t &pid, const std::string &man, const std::string &prod, const std::string &serial)
{
    Devices::USB::Core.changeUSBMode(mode, vid, pid, man, prod, serial);
}

void WaitTask(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(timeToWait));
    timeToWait = 0;
    vTaskDelete(NULL);
}

static void requestDelay(uint32_t time)
{
    // We can do other things while we are waiting so we kick off a task to wait
    // return, which causes us to loop again
    // in our loop we check if the task is finished and only continue processing if it has
    Debug::Log.info(LOG_DUCKY, "Requesting wait for " + std::to_string(time));

    timeToWait = time;

    xTaskCreate(
        WaitTask, // Function that should be called
        "Wait",   // Name of the task (for debugging)
        1000,     // Stack size (bytes)
        NULL,     // Parameter to pass
        1,        // Task priority
        NULL      // Task handle
    );
}

static DuckyInterpreter duckyFileParser = DuckyInterpreter(
    requestDelay,
    readLineFromFileOrCmdLine,
    keyboard_press,
    keyboard_release,
    changeLEDState,
    waitForButton,
    changeUSBMode,
    reset);

#include "Extensions.h"

uint8_t DuckyPayload::getTotalErrors()
{
    return totalErrors;
}

std::string DuckyPayload::getPayloadRunningStatus()
{
    if (lastExecutionResult == DuckyInterpreter::END_OF_FILE)
    {
        return "Complete";
    }
    else if (lastExecutionResult == DuckyInterpreter::SCRIPT_ERROR)
    {
        return "Error";
    }
    else if (!currentlyExecutingFile.empty() || !localCmdLineToExecute.empty())
    {
        return "Running";
    }
    else
    {
        return "Ready";
    }
}

void DuckyPayload::setPayload(const std::string &path)
{
    // Convert to std::string
    std::string newFileToExecute(path.c_str(), path.length());
    currentlyExecutingFile = newFileToExecute;
    Debug::Log.info(LOG_DUCKY, "Setting payload to - '" + currentlyExecutingFile + "'");
}

void DuckyPayload::setPayloadCmdLine(const std::string &cmdLine)
{
    localCmdLineToExecute = std::string(cmdLine.c_str(), cmdLine.length());
}

DuckyPayload::DuckyPayload()
{
}

void DuckyPayload::begin(Preferences &prefs)
{
    addDuckyScriptExtensions(extCommands, consts);
}

void DuckyPayload::loop(Preferences &prefs)
{
    if (timeToWait != 0)
    {
        // we are waiting for the wait task to complete
    }
    else if (!Devices::USB::HID.IsQueueEmpty())
    {
        // do nothing, yield to other event processing jobs
    }
    else if (requiresReset)
    {
        // perform any other flushing tasks
        Devices::USB::Core.end();
        ESP.restart();
    }
    else if (!currentlyExecutingFile.empty() || !localCmdLineToExecute.empty())
    {
        const bool executeFile = !currentlyExecutingFile.empty();

        if (!executeFile)
        {
            Debug::Log.info(LOG_DUCKY, "Executing cmdline: " + localCmdLineToExecute);
        }

        lastExecutionResult = duckyFileParser.Execute(executeFile ? currentlyExecutingFile : "", extCommands, consts);
        const bool executionHasCompleted = lastExecutionResult == DuckyInterpreter::SCRIPT_ERROR || lastExecutionResult == DuckyInterpreter::END_OF_FILE;

        if (executionHasCompleted)
        {
            // we are safe to clear both of these in whatever mode we are running in
            currentlyExecutingFile.clear(); 
            localCmdLineToExecute.clear();
        }

        if (lastExecutionResult == DuckyInterpreter::SCRIPT_ERROR)
        {
            Debug::Log.error(LOG_DUCKY, executeFile ? ("Script error near line " + std::to_string(lastExecutionResult + 1)) : "Error executing command");
            totalErrors++;
            
        }
        else if (lastExecutionResult == DuckyInterpreter::END_OF_FILE)
        {
            Debug::Log.info(LOG_DUCKY, "Script finished execution");
        }       
    }
    else if (firstRun)
    {
        if (Devices::Storage.getFileSize(AUTORUN_FILENAME) != 0)
        {
            setPayload(AUTORUN_FILENAME);
        }

        firstRun = false;
    }
}