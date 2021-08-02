//******************************************************************************************
// AquariumLedVerlichting for Wemos D1 (ESp8266).                                          *
// Powers 2 aquarium LED lights independantly.                                             *
// A webserver is available to set intensity and timer.                                    *
//******************************************************************************************
// 08-08-2021, ES - First setup                                                            *
//                                                                                         *
// Wiring:                                                                                 *
// GPIO    Wemos D1  Wired to                                                              *
// ------  --------  ----------------------------------------------------------------------*
// GPIO14    D5      PWM output Lamp A                                                     *
// GPIO12    D6      PWM output Lamp B                                                     *
//******************************************************************************************

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <stdio.h>
#include <string.h>
#include <LittleFS.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <mdns.h>
#include <Ticker.h>
#include <NTPClient.h>
#include <EEPROM.h>
#include <Timezone.h>    // https://github.com/JChristensen/Timezone

#define VERSION            "Wed, 28 Jul 2021 07:12:00 GMT"
#define DEBUG_BUFFER_SIZE  150                                // Line length for debugging
#define HTTPPORT            80                                // Port for HTTP communication
#define HOSTNAME    "AqLedVerl"                               // Hostname
#define LAMPA               D5                                // GPIO used for lamp A
#define LAMPB               D6                                // GPIO used for lamp B

ADC_MODE(ADC_VCC) ;                                           // Allow ADC to read VCC

// Forward declarations
const char* getEncryptionType ( int thisType ) ;

const int       DEBUG =   1 ;                                 // Output debug messages if ! 0

// Central European Time (Amsterdam, Frankfurt, Paris)
TimeChangeRule CEST = { "CEST", Last, Sun, Mar, 2, 120 } ;    // Central European Summer Time
TimeChangeRule CET  = { "CET ", Last, Sun, Oct, 3,  60 } ;    // Central European Standard Time
Timezone       myTZ ( CEST, CET ) ;                           // Timezone to be used

WiFiUDP              ntpUDP ;                                 // For NTP service
NTPClient            timeClient ( ntpUDP ) ;                  // For NTP service
mdns::MDns           my_mdns ( NULL, NULL, NULL ) ;           // mDNS without callbacks
Ticker               tckr ;                                   // For timing 1000 msec
String               ssid ;                                   // Network in use
AsyncWebServer*      httpserver;                              // Embedded webserver

std::vector<String>  dbglines ;                               // Container for last debug lines

struct set_t
{
  uint8_t            values[48] ;                             // Settings for 24 hours, 2 lamps
} ;
uint8_t              intensityA = 0 ;                         // Intensity lamp A 0..100
uint8_t              intensityB = 0 ;                         // Intensity lamp B 0..100
time_t               ltime ;                                  // Local time
set_t                settings ;                               // Settings for 2 x 24 hours
bool                 overrule = false ;                       // True for overrule normal intensity
uint8_t              ovA, ovB ;                               // Overrule intensities

//**************************************************************************************************
//                                          D B G P R I N T                                        *
//**************************************************************************************************
// Send a line of info to serial output.  Works like vsprintf(), but checks the DEBUG flag.        *
// Debug lines will be added to dbglines, a buffer holding the last debuglines.                    *
// Print only if DEBUG flag is true.                                                               *
//**************************************************************************************************
void dbgprint ( const char* format, ... )
{
  static char sbuf[DEBUG_BUFFER_SIZE] ;                // For debug lines
  char        tbuf[16] ;                               // Buffer for time of day
  va_list     varArgs ;                                // For variable number of params
  String      dbgline ;                                // Resulting line

  va_start ( varArgs, format ) ;                       // Prepare parameters
  vsnprintf ( sbuf, sizeof(sbuf), format, varArgs ) ;  // Format the message
  va_end ( varArgs ) ;                                 // End of using parameters
  if ( DEBUG )                                         // DEBUG on?
  {
    sprintf ( tbuf, "%02d:%02d:%02d - ",               // Convert time
              hour(ltime),
              minute(ltime),
              second(ltime) ) ;
    dbgline = String ( tbuf ) +                        // Format debugline
              String ( sbuf ) ;
    Serial.println ( dbgline ) ;                       // Yes, print info
    if ( ESP.getFreeHeap() > 8000 )                    // Memory shortage due to debug lines?
    {
      dbglines.push_back ( dbgline ) ;                 // No, add to buffer with debug lines
    }
  }
}


//******************************************************************************************
//                                  T I M E R 1 S E C                                      *
//******************************************************************************************
// Will be called every second.                                                            *
//******************************************************************************************
void timer1sec()
{
  ltime++ ;                               // Update local time
}


//******************************************************************************************
//                                L I S T N E T W O R K S                                  *
//******************************************************************************************
// List the available networks and select the strongest.                                   *
// Acceptable networks are those who have a "SSID.pw" file in the LittleFS.                *
//******************************************************************************************
void listNetworks()
{
  int         maxsig = -1000 ;   // Used for searching strongest WiFi signal
  int         newstrength ;
  byte        encryption ;       // TKIP(WPA)=2, WEP=5, CCMP(WPA)=4, NONE=7, AUTO=8 
  const char* acceptable ;       // Netwerk is acceptable for connection
  int         i ;
  String      path ;             // Full filespec to see if SSID is an acceptable one
  
  // scan for nearby networks:
  dbgprint ( "Scan Networks" ) ;
  int numSsid = WiFi.scanNetworks() ;
  if ( numSsid == -1 )
  {
    dbgprint ( "Couldn't get a wifi connection" ) ;
    return ;
  }
  // print the list of networks seen:
  dbgprint ( "Number of available networks: %d",
             numSsid ) ;
  // Print the network number and name for each network found and
  // find the strongest acceptable network
  for ( i = 0 ; i < numSsid ; i++ )
  {
    acceptable = "" ;                                    // Assume not acceptable
    path = String ( "/" ) + WiFi.SSID ( i ) + String ( ".pw" ) ;
    newstrength = WiFi.RSSI ( i ) ;
    if ( LittleFS.exists ( path ) )                      // Is this SSID acceptable?
    {
      acceptable = "Acceptable" ;
      if ( newstrength > maxsig )                        // This is a better Wifi
      {
        maxsig = newstrength ;
        ssid = WiFi.SSID ( i ) ;                         // Remember SSID name
      }
    }
    encryption = WiFi.encryptionType ( i ) ;
    dbgprint ( "%2d - %-25s Signal: %3d dBm Encryption %4s  %s",
               i + 1, WiFi.SSID ( i ).c_str(), WiFi.RSSI ( i ),
               getEncryptionType ( encryption ),
               acceptable ) ;
  }
  dbgprint ( "--------------------------------------" ) ;
  dbgprint ( "Selected network: %-25s", ssid.c_str() ) ;
}


//******************************************************************************************
//                             G E T E N C R Y P T I O N T Y P E                           *
//******************************************************************************************
// Read the encryption type of the network and return as a 4 byte name                     *
//******************************************************************************************
const char* getEncryptionType ( int thisType )
{
  switch (thisType) 
  {
    case ENC_TYPE_WEP:
      return "WEP " ;
    case ENC_TYPE_TKIP:
      return "WPA " ;
    case ENC_TYPE_CCMP:
      return "WPA2" ;
    case ENC_TYPE_NONE:
      return "None" ;
    case ENC_TYPE_AUTO:
      return "Auto" ;
  }
  return "????" ;
}


//******************************************************************************************
//                               C O N N E C T W I F I                                     *
//******************************************************************************************
// Connect to WiFi using passwords available in the LittleFS.                              *
//******************************************************************************************
void connectwifi()
{
  String path ;                                        // Full file spec
  String pw ;                                          // Password from file
  File   pwfile ;                                      // File containing password for WiFi
  
  path = String ( "/" )  + ssid + String ( ".pw" ) ;   // Form full path
  pwfile = LittleFS.open ( path, "r" ) ;                 // File name equal to SSID
  pw = pwfile.readStringUntil ( '\n' ) ;               // Read password as a string
  pw.trim() ;                                          // Remove CR                              
  WiFi.begin ( ssid.c_str(), pw.c_str() ) ;            // Connect to selected SSID
  dbgprint ( "Try WiFi %s",
             ssid.c_str() ) ;                          // Message to show during WiFi connect
  if (  WiFi.waitForConnectResult() != WL_CONNECTED )  // Try to connect
  {
    dbgprint ( "WiFi Failed!" ) ;
    return ;
  }
  dbgprint ( "IP = %d.%d.%d.%d",
              WiFi.localIP()[0], WiFi.localIP()[1],
              WiFi.localIP()[2], WiFi.localIP()[3] ) ;
}


//******************************************************************************************
//                             G E T C O N T E N T T Y P E                                 *
//******************************************************************************************
// Returns the contenttype of a file to send.                                              *
//******************************************************************************************
String getContentType ( String filename )
{
  if      ( filename.endsWith ( ".html" ) ) return "text/html" ;
  else if ( filename.endsWith ( ".png"  ) ) return "image/png" ;
  else if ( filename.endsWith ( ".gif"  ) ) return "image/gif" ;
  else if ( filename.endsWith ( ".jpg"  ) ) return "image/jpeg" ;
  else if ( filename.endsWith ( ".ico"  ) ) return "image/x-icon" ;
  else if ( filename.endsWith ( ".css"  ) ) return "text/css" ;
  else if ( filename.endsWith ( ".zip"  ) ) return "application/x-zip" ;
  else if ( filename.endsWith ( ".gz"   ) ) return "application/x-gzip" ;
  else if ( filename.endsWith ( ".pw"   ) ) return "" ;              // Passwords are secret
  return "text/plain" ;
}


//**************************************************************************************************
//                                        C B  _ L O G G I N G                                     *
//**************************************************************************************************
// Callback function for handle_logging, will be called for every chunk to send to client.         *
// If no more data is availble, this function will return 0.                                       *
//**************************************************************************************************
size_t cb_logging ( uint8_t *buffer, size_t maxLen, size_t index )
{
  static int   i ;                                   // Index in dbglines
  static int   nrl ;                                 // Mumber of lines in dbglines
  static char  linebuf[DEBUG_BUFFER_SIZE + 20] ;     // Holds one debug line
  static char* p_in ;                                // Pointer in linebuf
  char*        p_out = (char*)buffer ;               // Fill pointer for output buffer
  String       s ;                                   // Single line from dbglines
  size_t       len = 0 ;                             // Number of bytes filled in buffer
  
  if ( index == 0 )                                 // First call for this page?
  {
    i = 0 ;                                         // Yes, set index
    nrl = dbglines.size() ;                         // Number of lines in dbglines
    p_in = linebuf ;                                // Set linebuf to empty
    *p_in = '\0' ;
  }
  while ( maxLen-- > 0 )                            // Space for another character?
  {
    if ( *p_in == '\0' )                            // Input buffer end?
    {
      if ( i == nrl )                               // Yes, is there another line?
      {
        break ;                                     // No, end of text
      }
      s = dbglines[i++] ;                           // Yes, get next line from container
      strcpy ( linebuf, s.c_str() ) ;               // Fill linebuf
      strcat ( linebuf, "\n" ) ;                    // Add a break
      p_in = linebuf ;                              // Pointer to start of line
    }
    *p_out++ = *p_in++ ;                            // Copy next character
    len++ ;                                         // Increase result length
  }
  // We come here if output buffer is completely full or if end of dbglines is reached
  return len ;                                      // Return filled length of buffer
}


//**************************************************************************************************
//                                    H A N D L E _ L O G G I N G                                  *
//**************************************************************************************************
// Called from logging page to list the logging in dbglines.                                       *
// It will handle the chunks for the client.  The buffer is filled by the callback routine.        *
//**************************************************************************************************
void handle_logging ( AsyncWebServerRequest *request )
{
  AsyncWebServerResponse *response ;

  dbgprint ( "HTTP logging request" ) ;
  response = request->beginChunkedResponse ( "text/plain", cb_logging ) ;
  response->addHeader ( "Server", HOSTNAME ) ;
  request->send ( response ) ;
}


//******************************************************************************************
//                                   H A N D L E _ T E S T                                 *
//******************************************************************************************
// Handle Test button.                                                                     *
//******************************************************************************************
void handle_test ( AsyncWebServerRequest *request )
{
  static char        reply[32] ;                        // Reply to client

  sprintf ( reply, "Free memory is %d",                 // Testing
            ESP.getFreeHeap() ) ;
  dbgprint ( reply ) ;
  request->send ( 200, "text/plain", reply ) ;
}


//******************************************************************************************
//                                H A N D L E _ R O O T                                    *
//******************************************************************************************
// Handle homepage.                                                                        *
//******************************************************************************************
void handle_root ( AsyncWebServerRequest *request )
{
  request->send ( LittleFS, "/index.html" ) ;
}


//******************************************************************************************
//                                H A N D L E _ R E S E T                                  *
//******************************************************************************************
// Handle reset request.                                                                   *
//******************************************************************************************
void handle_reset ( AsyncWebServerRequest *request )
{
  ESP.reset() ;
}


//******************************************************************************************
//                             H A N D L E _ G E T C O N F                                 *
//******************************************************************************************
// Handle get configuration request.                                                       *
// Return a string with 48 settings.                                                       *
//******************************************************************************************
void handle_getconf ( AsyncWebServerRequest *request )
{
  String    reply = "" ;                                // Reply to client
  int       i ;                                         // Loop control

  dbgprint ( "HTTP getconf request" ) ;
  for ( i = 0 ; i < 48 ; i++ )                          // Settings for 24 hours, 2 lamps
  {
    reply += String ( settings.values[i] ) ;            // Add setting
    reply += String ( ',' ) ;                           // Separator
  }
  request->send ( 200, "text/plain", reply ) ;
}


//******************************************************************************************
//                             H A N D L E _ S E T C O N F                                 *
//******************************************************************************************
// Handle set configuration request.                                                       *
// parameter is a string with 48 settings, separated by a comma                            *
//******************************************************************************************
void handle_setconf ( AsyncWebServerRequest *request )
{
  AsyncWebParameter* p ;                                // Points to parameter structure
  String             value ;                            // Parameter value
  int                i ;                                // Loop control
  int                inx ;                              // Position of next comma

  dbgprint ( "HTTP setconf request" ) ;
  p = request->getParam ( 0 ) ;                         // Get pointer to parameter structure
  value = p->value() ;                                  // Get value
  for ( i = 0 ; i < 48 ; i++ )                          // Settings for 24 hours, 2 lamps
  {
    settings.values[i] = value.toInt() ;                // Get next setting
    inx = value.indexOf ( "," ) ;                       // Find comma in string
    if ( inx > 0 )
    {
      value = value.substring ( inx + 1 ) ;             // Skip to next integer value
    }
  }
  overrule = false ;                                    // No more overrule
  EEPROM.put ( 0, settings ) ;                          // Save in EEPROM
  EEPROM.commit() ;                                     // And commint
  request->send ( 200, "text/plain",                    // Reply
                       "SET command accepted" ) ;
}


//******************************************************************************************
//                           H A N D L E _ O V E R R U L E                                 *
//******************************************************************************************
// Handle overlue normal intensity settings.                                               *
// Parameter is a string with 2 settings, separated by a comma                             *
//******************************************************************************************
void handle_overrule ( AsyncWebServerRequest *request )
{
  AsyncWebParameter* p ;                                // Points to parameter structure
  String             value ;                            // Parameter value
  int                inx ;                              // Position of next comma

  dbgprint ( "HTTP overrule request" ) ;
  p = request->getParam ( 0 ) ;                         // Get pointer to parameter structure
  value = p->value() ;                                  // Get value
  overrule = true ;                                     // Set overrule flag
  ovA = value.toInt() ;                                 // Set overule lamp A value
  inx = value.indexOf ( "," ) ;                         // Find comma in string
  if ( inx > 0 )
  {
    value = value.substring ( inx + 1 ) ;               // Skip to next integer value
    ovB = value.toInt() ;                               // Set overul lamp B value
  }
  request->send ( 200, "text/plain",                    // Reply
                       "Overrule command accepted" ) ;
}


//******************************************************************************************
//                              O N F I L E R E Q U E S T                                  *
//******************************************************************************************
// Handle requests for file/html-pages.                                                    *
// The requested filename is in url.                                                       *
//******************************************************************************************
void onFileRequest ( AsyncWebServerRequest *request )
{
  String fnam ;
  String ct ;                                           // Content type

  fnam = request->url() ;
  dbgprint ( "onFileRequest received %s",
             fnam.c_str() ) ;
  ct = getContentType ( fnam ) ;                        // Get content type
  if ( ct == "" )                                       // Empty is illegal
  {
    request->send ( 404, "text/plain", "File not found" ) ;  
  }
  else
  {
    request->send ( LittleFS, fnam, ct ) ;                // Okay, send the file
  }
}


//******************************************************************************************
//                                   O T A S T A R T                                       *
//******************************************************************************************
// Update via WiFi has been started by Arduino IDE.                                        *
//******************************************************************************************
void otastart()
{
  dbgprint ( "OTA Started" ) ;
}


//******************************************************************************************
//                                   S E T U P                                             *
//******************************************************************************************
// Setup for the program.                                                                  *
//******************************************************************************************
void setup()
{
  FSInfo      fs_info ;                              // LittleFS info
  Dir         dir ;

  Serial.begin ( 115200 ) ;                          // For debugging
  Serial.println ( ) ;
  EEPROM.begin ( 512 ) ;                             // Enable EEPROM
  EEPROM.get ( 0, settings ) ;                       // Get settings from EEPROM
  dbgprint ( "Starting " HOSTNAME "..." ) ;          // Show activity
  dbgprint ( "Version " VERSION ) ;
  pinMode ( LED_BUILTIN, OUTPUT ) ;                  // Configure onboard LED pin
  pinMode ( LAMPA, OUTPUT ) ;                        // Configure LED lamp A
  pinMode ( LAMPB, OUTPUT ) ;                        // Configure LED lamp B
  analogWriteRange ( 100 ) ;                         // PWM range 0..100 percent
  digitalWrite ( LED_BUILTIN, LOW ) ;                // Show LED for test
  delay ( 500 ) ;                                    // For at least 500 msec
  // Show some info about the LittleFS
  LittleFS.begin() ;                                 // Enable file system
  LittleFS.info ( fs_info ) ;
  dbgprint ( "FS Total %d, used %d",                 // Show FS overview
             fs_info.totalBytes,
             fs_info.usedBytes ) ;
  dir = LittleFS.openDir ( "/" ) ;                   // Show files in FS
  while ( dir.next() )                               // All files
  {
    String filename = dir.fileName() ;
    if ( dir.fileSize() )
    {
      File f = dir.openFile ( "r" ) ;
      dbgprint ( "%-32s - %6d",                      // Show name and size
                 filename.c_str(), f.size() ) ;
    }
  }
  WiFi.mode ( WIFI_STA ) ;                           // This ESP is a station
  wifi_station_set_hostname ( HOSTNAME ) ;           // Set hostname
  listNetworks() ;
  connectwifi() ;
  if ( DEBUG )
  {
    Serial.println ( ) ;
    Serial.println ( "WiFi connected" ) ;
    Serial.print   ( "Local   IP address: " ) ;
    Serial.println ( WiFi.localIP() ) ;
    Serial.print   ( "Gateway IP address: " ) ;
    Serial.println ( WiFi.gatewayIP() ) ;
  }
  httpserver = new AsyncWebServer ( HTTPPORT ) ;     // Create HTTP server
  httpserver->on ( "/",         handle_root ) ;      // Homepage request
  httpserver->on ( "/logging",  handle_logging ) ;   // Handle logging by a callback
  httpserver->on ( "/getconf",  handle_getconf ) ;   // Handle get configuration
  httpserver->on ( "/setconf",  handle_setconf ) ;   // Handle get configuration
  httpserver->on ( "/overrule", handle_overrule ) ;  // Handle get configuration
  httpserver->on ( "/reset",    handle_reset ) ;     // Handle reset request
  httpserver->on ( "/test",     handle_test ) ;      // Handle test request
  httpserver->onNotFound ( onFileRequest ) ;         // Handling of other requests
  httpserver->begin() ;                              // Start http server
  dbgprint ( "HTTP-server started on port %d",       // Show event
             HTTPPORT ) ;
  ArduinoOTA.setHostname ( HOSTNAME ) ;              // Set the hostname
  ArduinoOTA.onStart ( otastart ) ;
  ArduinoOTA.begin() ;                               // Allow update over the air
  timeClient.begin() ;                               // Enable NTP service
  tckr.attach ( 1.0, timer1sec ) ;                   // Every 1000 msec
  digitalWrite ( LED_BUILTIN, HIGH ) ;               // Turn LED off
}


//******************************************************************************************
//                                   L O O P                                               *
//******************************************************************************************
// Main loop of the program.                                                               *
//******************************************************************************************
void loop()
{
  String          ftm ;                                     // Formatted time
  time_t          utc ;                                     // Epochtime (utc)
  static bool     time_ok = false ;                         // Time is okay
  static uint32_t rfrltm = 0 ;                              // Timer for refresh local time
  uint32_t        millisnow ;                               // Current value of 
  uint8_t         inx ;                                     // Index in settings
  uint8_t         newA ;                                    // New intensity for lamp A
  uint8_t         newB ;                                    // New intensity for lamp B

  millisnow = millis() ;                                    // Get runtime
  time_ok = timeClient.update() ;                           // Update time
  if ( time_ok )                                            // Do we know the time?
  {
    if ( millisnow > rfrltm )                               // Yes, need for refresh?
    {
      rfrltm = millisnow * 1000 * 600 ;                     // Yes, set new refreshmoment
      utc = timeClient.getEpochTime() ;
      ltime = myTZ.toLocal ( utc ) ;                        // Convert to local
    }
  }
  if ( overrule )                                           // Overrule timed setting?
  {
    newA = ovA ;                                            // Yes, get new setting
    newB = ovB ;
  }
  else
  {
    inx = hour ( ltime ) ;                                  // Index in settings (0..47)
    newA = settings.values[inx*2] ;                         // Get intensity lamp A
    newB = settings.values[inx*2+1] ;                       // Get intensity lamp B
  }
  if ( newA != intensityA )                                 // Lamp A change needed?
  {
    intensityA = newA ;                                     // Yes, remember new value
    analogWrite ( LAMPA, intensityA ) ;                     // Set intensity lamp A
  }
  if ( newB != intensityB )                                 // Lamp B change needed?
  {
    intensityB = newB ;                                     // Yes, remember new value
    analogWrite ( LAMPB, intensityB ) ;                     // Set intensity lamp B
  }
  my_mdns.loop() ;                                          // Handle mDNS
  ArduinoOTA.handle() ;                                     // Check for OTA
}
