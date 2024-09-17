/* DirectSAPI1 is 8-bit computer emulator based on the FabGL library
 * -> see http://www.fabglib.org/ or FabGL on GitHub.
 *  
 * For proper operation, an ESP32 module with a VGA monitor 
 * and a PS2 keyboard connected according to the 
 * diagram on the website www.fabglib.org is required.
 * 
 * Cassette recorder is emulated using SPIFFS. The user can save his programs 
 * in SPIFFS in the form of files with the extension .BAS. Name of file 
 * is inserted after LOAD or SAVE command without extension. Extension .BAS is  
 * added automatically. When MCP23S17 is presented 2 ports expand
 * computer - address 6 (direction 7) and address 8 (direction 9).
 * 
 * DirectSAPI1 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or any later version.
 * DirectSAPI1 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY.
 * Stan Pechal, 2024
 * Version 1.0
*/
#include "fabgl.h"
#include "emudevs/i8080.h"          // For processor
#include "devdrivers/MCP23S17.h"    // For expansion ports
#include "SPIFFS.h"

fabgl::VGADirectController DisplayController;
fabgl::PS2Controller PS2Controller;

// Constants for video output
static constexpr int borderSize           = 30;
static constexpr int borderXSize          = 80;
static constexpr int scanlinesPerCallback = 2;  // screen height should be divisible by this value

static TaskHandle_t  mainTaskHandle;
void * auxPoint;

// **************************************************************************************************
// "Tape recorder" is emulated by files .BAS in SPIFFS
File file;
String fileName;          // Name of saved/opened file
bool isTape = false;      // Is SPIFFS ready
bool doLoad = false;      // Flags for load/save
bool loadFileFlag = false;

// Hardware emulated on the SAPI1 computer
// Processor I8080 will be used from the library FabGL
fabgl::i8080 m_i8080;
// Variables for emulating keyboard connection
int portP0 = 0xFF;    // Data sent to P0 port
int keyboardIn[5];    // Value read from keyboard P0 port
int portP1 = 0xFF;    // Data read from P1 port
// MCP23S17 can emulate 2 ports on board (if present)
fabgl::MCP23S17  mcp2317;
bool readyMCP2317 = false;
// variables for video AND1 display
bool blinkFlag = false;   // Flag for pixel blinking
int BlinkCnt=0;   // Counter for video pixel blink
uint8_t fgcolor; // character color
uint8_t bgcolor; // AND1 background color
uint8_t darkbgcolor; // backplane
uint8_t led1dcolor, led1bcolor, led2dcolor, led2bcolor; // colors for LEDs on display
int width, height;  // display dimensions

// RAM memory will be just Byte array
uint8_t SAPI1ram[32768];   // selected addresses are overwritten by ROM in read mode
// ROM memory is contained in the array "JPR1rom[]"
#include "JPR1rom.h"
// ROM memory of character generator is in the array "MHB2501rom[]"
#include "MHB2501font.h"

// **************************************************************************************************
// Functions for communication on the bus
static int readByte(void * context, int address)              { if(address < 0x1000) return(JPR1rom[address]); 
                                                                else if ((address >= 0x2400) && (address < 0x2800)) {
                                                                  if(!(portP0 & 0x01)) return keyboardIn[0]; 
                                                                  if(!(portP0 & 0x02)) return keyboardIn[1]; 
                                                                  if(!(portP0 & 0x04)) return keyboardIn[2]; 
                                                                  if(!(portP0 & 0x08)) return keyboardIn[3]; 
                                                                  if(!(portP0 & 0x10)) return keyboardIn[4]; 
                                                                }
                                                                else if ((address >= 0x2800) && (address < 0x2C00)) {
                                                                  return portP1; 
                                                                }
                                                                else return(SAPI1ram[address & 0x7FFF]); };
static void writeByte(void * context, int address, int value) { if ((address >= 0x2400) && (address < 0x2800)) { 
                                                                    portP0 = (unsigned char)value;
                                                                    if(value & 0x40) digitalWrite(25, HIGH); else digitalWrite(25, LOW);      // Audio output is very simple :-) ... so it's not perfect
                                                                }
                                                                else SAPI1ram[address & 0x7FFF] = (unsigned char)value; };
static int readWord(void * context, int addr)                 { return readByte(context, addr) | (readByte(context, addr + 1) << 8); };
static void writeWord(void * context, int addr, int value)    { writeByte(context, addr, value & 0xFF); writeByte(context, addr + 1, value >> 8); } ;
// Input/Output is emulated only if MCP23S17 is connected
static int readIO(void * context, int address)
{
  switch (address) {
    // *** MCP23S17 (if presented)
    case 0x06:      // PortA
      if(readyMCP2317) return mcp2317.readPort(MCP_PORTA); else return 0xFF;
    break;
    case 0x08:      // PortB
      if(readyMCP2317) return mcp2317.readPort(MCP_PORTB); else return 0xFF;
    break;
    default: return 0xFF; break;  // Return "not selected I/O" - bus with 0xFF
  }
};
static void writeIO(void * context, int address, int value)
{
    switch (address) {
    // *** MCP23S17 (if presented)
    case 0x06:      // PortA
      if(readyMCP2317) mcp2317.writePort(MCP_PORTA,(uint8_t)value);
    break;
    case 0x07:      // PortA direction and pull-up
      if(readyMCP2317) { mcp2317.setPortDir(MCP_PORTA,(uint8_t)value); mcp2317.enablePortPullUp(MCP_PORTA,(uint8_t)value); }
    break;
    case 0x08:      // PB port
      if(readyMCP2317) mcp2317.writePort(MCP_PORTB,(uint8_t)value);
    break;
    case 0x09:      // PortB direction and pull-up
      if(readyMCP2317) { mcp2317.setPortDir(MCP_PORTB,(uint8_t)value); mcp2317.enablePortPullUp(MCP_PORTB,(uint8_t)value); }
    break;
    default: break;
  }
};

// **************************************************************************************************
// Keyboard interface for selected keys
// Handles Key Up following keys:
void procesKeyUp(VirtualKey key) {
  switch (key) {
      case VirtualKey::VK_F1: portP1 |= 0x80; break;  // F1 as INT
      case VirtualKey::VK_F2: portP1 |= 0x40; break;  // F2 as T

      case VirtualKey::VK_KP_0:
      case VirtualKey::VK_RIGHTPAREN:
      case VirtualKey::VK_0: keyboardIn[4] |= 0x80; break;  // 0
      case VirtualKey::VK_KP_1:
      case VirtualKey::VK_EXCLAIM:
      case VirtualKey::VK_1: keyboardIn[4] |= 0x08; break;  // 1
      case VirtualKey::VK_KP_2:
      case VirtualKey::VK_AT:
      case VirtualKey::VK_2: keyboardIn[3] |= 0x08; break;  // 2
      case VirtualKey::VK_KP_3:
      case VirtualKey::VK_HASH:
      case VirtualKey::VK_3: keyboardIn[2] |= 0x08; break;  // 3
      case VirtualKey::VK_KP_4:
      case VirtualKey::VK_DOLLAR:
      case VirtualKey::VK_4: keyboardIn[1] |= 0x08; break;  // 4
      case VirtualKey::VK_KP_5:
      case VirtualKey::VK_PERCENT:
      case VirtualKey::VK_5: keyboardIn[0] |= 0x08; break;  // 5
      case VirtualKey::VK_KP_6:
      case VirtualKey::VK_CARET:
      case VirtualKey::VK_6: keyboardIn[0] |= 0x80; break;  // 6
      case VirtualKey::VK_KP_7:
      case VirtualKey::VK_AMPERSAND:
      case VirtualKey::VK_7: keyboardIn[1] |= 0x80; break;  // 7
      case VirtualKey::VK_KP_8:
      case VirtualKey::VK_ASTERISK:
      case VirtualKey::VK_8: keyboardIn[2] |= 0x80; break;  // 8
      case VirtualKey::VK_KP_9:
      case VirtualKey::VK_LEFTPAREN:
      case VirtualKey::VK_9: keyboardIn[3] |= 0x80; break;  // 9

      case VirtualKey::VK_q:
      case VirtualKey::VK_Q: keyboardIn[4] |= 0x04; break;  // q-Q
      case VirtualKey::VK_w:
      case VirtualKey::VK_W: keyboardIn[3] |= 0x04; break;  // w-W
      case VirtualKey::VK_e:
      case VirtualKey::VK_E: keyboardIn[2] |= 0x04; break;  // e-E
      case VirtualKey::VK_r:
      case VirtualKey::VK_R: keyboardIn[1] |= 0x04; break;  // r-R
      case VirtualKey::VK_t:
      case VirtualKey::VK_T: keyboardIn[0] |= 0x04; break;  // t-T
      case VirtualKey::VK_y:
      case VirtualKey::VK_Y: keyboardIn[0] |= 0x40; break;  // y-Y
      case VirtualKey::VK_u:
      case VirtualKey::VK_U: keyboardIn[1] |= 0x40; break;  // u-U
      case VirtualKey::VK_i:
      case VirtualKey::VK_I: keyboardIn[2] |= 0x40; break;  // i-I
      case VirtualKey::VK_o:
      case VirtualKey::VK_O: keyboardIn[3] |= 0x40; break;  // o-O
      case VirtualKey::VK_p:
      case VirtualKey::VK_P: keyboardIn[4] |= 0x40; break;  // p-P

      case VirtualKey::VK_a:
      case VirtualKey::VK_A: keyboardIn[4] |= 0x02; break;  // a-A
      case VirtualKey::VK_s:
      case VirtualKey::VK_S: keyboardIn[3] |= 0x02; break;  // s-S
      case VirtualKey::VK_d:
      case VirtualKey::VK_D: keyboardIn[2] |= 0x02; break;  // d-D
      case VirtualKey::VK_f:
      case VirtualKey::VK_F: keyboardIn[1] |= 0x02; break;  // f-F
      case VirtualKey::VK_g:
      case VirtualKey::VK_G: keyboardIn[0] |= 0x02; break;  // g-G
      case VirtualKey::VK_h:
      case VirtualKey::VK_H: keyboardIn[0] |= 0x20; break;  // h-H
      case VirtualKey::VK_j:
      case VirtualKey::VK_J: keyboardIn[1] |= 0x20; break;  // j-J
      case VirtualKey::VK_k:
      case VirtualKey::VK_K: keyboardIn[2] |= 0x20; break;  // k-K
      case VirtualKey::VK_l:
      case VirtualKey::VK_L: keyboardIn[3] |= 0x20; break;  // l-L
      case VirtualKey::VK_RETURN:
      case VirtualKey::VK_KP_ENTER: keyboardIn[4] |= 0x20; break;  // R Enter

      case VirtualKey::VK_LSHIFT:
      case VirtualKey::VK_RSHIFT: keyboardIn[4] |= 0x01; break;  // L and R shift
      case VirtualKey::VK_z:
      case VirtualKey::VK_Z: keyboardIn[3] |= 0x01; break;  // z-Z
      case VirtualKey::VK_x:
      case VirtualKey::VK_X: keyboardIn[2] |= 0x01; break;  // x-X
      case VirtualKey::VK_c:
      case VirtualKey::VK_C: keyboardIn[1] |= 0x01; break;  // c-C
      case VirtualKey::VK_v:
      case VirtualKey::VK_V: keyboardIn[0] |= 0x01; break;  // v-V
      case VirtualKey::VK_b:
      case VirtualKey::VK_B: keyboardIn[0] |= 0x10; break;  // b-B
      case VirtualKey::VK_n:
      case VirtualKey::VK_N: keyboardIn[1] |= 0x10; break;  // n-N
      case VirtualKey::VK_m:
      case VirtualKey::VK_M: keyboardIn[2] |= 0x10; break;  // m-M
      case VirtualKey::VK_SPACE: keyboardIn[3] |= 0x10; break;  // space
      case VirtualKey::VK_LCTRL:
      case VirtualKey::VK_RCTRL: keyboardIn[4] |= 0x10; break;  // LF
      default: break;
      }
};

// Handles Key Down following keys:
void procesKeyDown(VirtualKey key) {
  switch (key) {
      case VirtualKey::VK_ESCAPE: m_i8080.reset(); break;   // ESC as RESET
      case VirtualKey::VK_F1: portP1 &= 0x7F; break;        // F1 as INT
      case VirtualKey::VK_F2: portP1 &= 0xBF; break;        // F2 as T

      case VirtualKey::VK_KP_0:
      case VirtualKey::VK_RIGHTPAREN:
      case VirtualKey::VK_0: keyboardIn[4] &= 0x7F; break;  // 0
      case VirtualKey::VK_KP_1:
      case VirtualKey::VK_EXCLAIM:
      case VirtualKey::VK_1: keyboardIn[4] &= 0xF7; break;  // 1
      case VirtualKey::VK_KP_2:
      case VirtualKey::VK_AT:
      case VirtualKey::VK_2: keyboardIn[3] &= 0xF7; break;  // 2
      case VirtualKey::VK_KP_3:
      case VirtualKey::VK_HASH:
      case VirtualKey::VK_3: keyboardIn[2] &= 0xF7; break;  // 3
      case VirtualKey::VK_KP_4:
      case VirtualKey::VK_DOLLAR:
      case VirtualKey::VK_4: keyboardIn[1] &= 0xF7; break;  // 4
      case VirtualKey::VK_KP_5:
      case VirtualKey::VK_PERCENT:
      case VirtualKey::VK_5: keyboardIn[0] &= 0xF7; break;  // 5
      case VirtualKey::VK_KP_6:
      case VirtualKey::VK_CARET:
      case VirtualKey::VK_6: keyboardIn[0] &= 0x7F; break;  // 6
      case VirtualKey::VK_KP_7:
      case VirtualKey::VK_AMPERSAND:
      case VirtualKey::VK_7: keyboardIn[1] &= 0x7F; break;  // 7
      case VirtualKey::VK_KP_8:
      case VirtualKey::VK_ASTERISK:
      case VirtualKey::VK_8: keyboardIn[2] &= 0x7F; break;  // 8
      case VirtualKey::VK_KP_9:
      case VirtualKey::VK_LEFTPAREN:
      case VirtualKey::VK_9: keyboardIn[3] &= 0x7F; break;  // 9


      case VirtualKey::VK_q:
      case VirtualKey::VK_Q: keyboardIn[4] &= 0xFB; break;  // q-Q
      case VirtualKey::VK_w:
      case VirtualKey::VK_W: keyboardIn[3] &= 0xFB; break;  // w-W
      case VirtualKey::VK_e:
      case VirtualKey::VK_E: keyboardIn[2] &= 0xFB; break;  // e-E
      case VirtualKey::VK_r:
      case VirtualKey::VK_R: keyboardIn[1] &= 0xFB; break;  // r-R
      case VirtualKey::VK_t:
      case VirtualKey::VK_T: keyboardIn[0] &= 0xFB; break;  // t-T
      case VirtualKey::VK_y:
      case VirtualKey::VK_Y: keyboardIn[0] &= 0xBF; break;  // y-Y
      case VirtualKey::VK_u:
      case VirtualKey::VK_U: keyboardIn[1] &= 0xBF; break;  // u-U
      case VirtualKey::VK_i:
      case VirtualKey::VK_I: keyboardIn[2] &= 0xBF; break;  // i-I
      case VirtualKey::VK_o:
      case VirtualKey::VK_O: keyboardIn[3] &= 0xBF; break;  // o-O
      case VirtualKey::VK_p:
      case VirtualKey::VK_P: keyboardIn[4] &= 0xBF; break;  // p-P

      case VirtualKey::VK_a:
      case VirtualKey::VK_A: keyboardIn[4] &= 0xFD; break;  // a-A
      case VirtualKey::VK_s:
      case VirtualKey::VK_S: keyboardIn[3] &= 0xFD; break;  // s-S
      case VirtualKey::VK_d:
      case VirtualKey::VK_D: keyboardIn[2] &= 0xFD; break;  // d-D
      case VirtualKey::VK_f:
      case VirtualKey::VK_F: keyboardIn[1] &= 0xFD; break;  // f-F
      case VirtualKey::VK_g:
      case VirtualKey::VK_G: keyboardIn[0] &= 0xFD; break;  // g-G
      case VirtualKey::VK_h:
      case VirtualKey::VK_H: keyboardIn[0] &= 0xDF; break;  // h-H
      case VirtualKey::VK_j:
      case VirtualKey::VK_J: keyboardIn[1] &= 0xDF; break;  // j-J
      case VirtualKey::VK_k:
      case VirtualKey::VK_K: keyboardIn[2] &= 0xDF; break;  // k-K
      case VirtualKey::VK_l:
      case VirtualKey::VK_L: keyboardIn[3] &= 0xDF; break;  // l-L
      case VirtualKey::VK_RETURN:
      case VirtualKey::VK_KP_ENTER: keyboardIn[4] &= 0xDF; break;  // R Enter

      case VirtualKey::VK_LSHIFT:
      case VirtualKey::VK_RSHIFT: keyboardIn[4] &= 0xFE; break;  // L and R shift
      case VirtualKey::VK_z:
      case VirtualKey::VK_Z: keyboardIn[3] &= 0xFE; break;  // z-Z
      case VirtualKey::VK_x:
      case VirtualKey::VK_X: keyboardIn[2] &= 0xFE; break;  // x-X
      case VirtualKey::VK_c:
      case VirtualKey::VK_C: keyboardIn[1] &= 0xFE; break;  // c-C
      case VirtualKey::VK_v:
      case VirtualKey::VK_V: keyboardIn[0] &= 0xFE; break;  // v-V
      case VirtualKey::VK_b:
      case VirtualKey::VK_B: keyboardIn[0] &= 0xEF; break;  // b-B
      case VirtualKey::VK_n:
      case VirtualKey::VK_N: keyboardIn[1] &= 0xEF; break;  // n-N
      case VirtualKey::VK_m:
      case VirtualKey::VK_M: keyboardIn[2] &= 0xEF; break;  // m-M
      case VirtualKey::VK_SPACE: keyboardIn[3] &= 0xEF; break;  // space
      case VirtualKey::VK_LCTRL:
      case VirtualKey::VK_RCTRL: keyboardIn[4] &= 0xEF; break;  // LF
      default: break;
      }
};

// **************************************************************************************************
// VGA main function - prepare lines for displaying
void IRAM_ATTR drawScanline(void * arg, uint8_t * dest, int scanLine)
{
  // draws "scanlinesPerCallback" scanlines every time drawScanline() is called
  for (int i = 0; i < scanlinesPerCallback; ++i) {
    // fill border with background color
    memset(dest, darkbgcolor, width);
    if ((scanLine < borderSize) || (scanLine >= (240 + borderSize)))     // Black stripe up and bottom
      {
        if ((scanLine < 285) && (scanLine >279)) {    // LEDs of ANK1 are displayed on these rows
          for(int i=0; i<5; i++) {
            if(portP0 & 0x80) VGA_PIXELINROW(dest, 150+i) = led1bcolor; else VGA_PIXELINROW(dest, 150+i) = led1dcolor;
            if(portP0 & 0x20) VGA_PIXELINROW(dest, 245+i) = led2bcolor; else VGA_PIXELINROW(dest, 245+i) = led2dcolor;
          }
        }
      }
    else    // Display is 24 char lines * 10 pixel rows hight
      { // Prepare row with background color
        memset(dest+borderXSize, bgcolor, 240);
        int displayLine = scanLine - borderSize;
        int pixelPointer = 0;   // pointer to pixel on a row
        int memoryPointer = 0x3800 + ((displayLine / 10) << 6);   // Start of line in memory
        do {
          uint8_t videobyte = SAPI1ram[memoryPointer];  // Get byte to display
          int lineInChar = displayLine % 10;        // Number of row in character
          uint8_t lineChar = MHB2501rom[((videobyte & 0x3F) << 3) + lineInChar];  // Row in character
          if((videobyte & 0xC0) == 0xC0) { // Double size of character (wide)
            if(lineInChar < 7) {
                if(lineChar & 0x10) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                if(lineChar & 0x10) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                if(lineChar & 0x08) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                if(lineChar & 0x08) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                if(lineChar & 0x04) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                if(lineChar & 0x04) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                if(pixelPointer < 235) { // Not finished character at end of line
                  if(lineChar & 0x02) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                  if(lineChar & 0x02) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                  if(lineChar & 0x01) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                  if(lineChar & 0x01) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                  pixelPointer++; pixelPointer++;    // Empty column between chars
                }
            }
          } else {    // Normal character
          if(lineInChar < 7) {
            // Set 6 pixel on the screen
            if ((videobyte & 0x40) && blinkFlag) {    // If blinking - do not display character
              pixelPointer+=6;
            } else {  // Display normal (narrow) character
                if(lineChar & 0x10) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                if(lineChar & 0x08) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                if(lineChar & 0x04) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                if(lineChar & 0x02) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                if(lineChar & 0x01) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;
                pixelPointer++;     // Empty column between chars
             }
          } else 
            if(lineInChar == 8) {  // Cursor on the 8th line of normal character
              if((videobyte & 0x80) && !blinkFlag)
                { for(int m=0; m<5; m++) {VGA_PIXELINROW(dest, pixelPointer+borderXSize) = fgcolor; pixelPointer++;} pixelPointer++; }
              else pixelPointer+=6;  
            } else pixelPointer+=6;
          }
          memoryPointer++;
        } while (pixelPointer < 239);
    }
    // go to next scanline
    ++scanLine;
    dest += width;
  }
  if (scanLine == height) {
    // signal end of screen
    vTaskNotifyGiveFromISR(mainTaskHandle, NULL);
  }
}

// **************************************************************************************************
// Load or save file from SPIFFS instead of cassete recorder
bool diskFile()
{
  // Create file name at first
  fileName = "/";
  unsigned int namePoint = (unsigned int)SAPI1ram[0x40EB] + ((unsigned int)SAPI1ram[0x40EC] << 8);      // Name of loaded file starts here
  if((SAPI1ram[namePoint] < 0x41) || (SAPI1ram[namePoint] > 0x5A)) return false;  // Name must start with letter
  unsigned int k=0;  // counter for parsing file name in SAPI memory
  do{
    fileName += (char)SAPI1ram[namePoint+k]; k++;      // Parse file name
  } while((SAPI1ram[namePoint+k] != 0x0D) && (k<10));    // 10 characters max and CR is stop
  fileName += ".BAS";       // Add suffix

  if(isTape) {      // "Tape" recorder on SPIFFS must be ready
    uint8_t* startAddr = SAPI1ram + 0x40EF;     // Pointer to memory, where BASIC program starts
    if(doLoad) {  // And fork LOAD / SAVE operation
      // If SPIFFS is ready, try open file
      file = SPIFFS.open(fileName, "rb");
      if(file){ // Error will exit file load
        int maximalSize = ((unsigned int)SAPI1ram[0x40E9] + ((unsigned int)SAPI1ram[0x40EA] << 8)) - 0x40EF;  // This is maximal size of program
        int fileSize = file.size();             // Get size of file
        if(fileSize < maximalSize) {            // Test if not crossed the end of the memory
          fileSize = file.readBytes((char*)startAddr, fileSize);   // Read file to memory
          int bytesGet = fileSize + 0x40EF;     // End address of loaded file in RAM
          SAPI1ram[0x4024] = (uint8_t)bytesGet; SAPI1ram[0x4025] = (uint8_t)(bytesGet >> 8);    // And set pointer to end of program
          file.close();
          if(fileSize > 0) return true;  else return false;   // Success or error in load function
        } else { file.close(); return false;}  // Too long file
      } else return false;    // Not opened file
    } 
    else {  // Here SAVE operation
      int endFilePoint = ((int)SAPI1ram[0x4024] + ((int)SAPI1ram[0x4025] << 8)); // End of program memory
      if(endFilePoint == 0x40EF) return false;      // Not data to save
      int bytesSave = endFilePoint - 0x40EF;        // How many bytes to save
      file = SPIFFS.open(fileName, "wb");
      if(file){ // Error will exit file save
        if(file.write(startAddr, bytesSave)) {
          file.close();
          return true;  // Success in save function
        } else { file.close(); return false; }  // Error in write
      } else return false;     // Not opened file
    }
  } else return false;    // No "tape recorder"
}

// **************************************************************************************************
void setup()
{
  mainTaskHandle = xTaskGetCurrentTaskHandle();

  // Audio output
  pinMode(25, OUTPUT);    // Audio output
  pinMode(34, OUTPUT);    // Auxiliary output

  // "Tape recorder" on SPIFFS
  if(SPIFFS.begin(true)) isTape = true;
  // Expansion ports
  if(mcp2317.begin()) { 
    readyMCP2317 = true;
    mcp2317.enablePortPullUp(MCP_PORTA,0xFF); // Pull-up on inputs
    mcp2317.enablePortPullUp(MCP_PORTB,0xFF);
  }

  // Set VGA for SAPI1 display grey monitor
  DisplayController.begin();
  DisplayController.setScanlinesPerCallBack(scanlinesPerCallback);
  DisplayController.setDrawScanlineCallback(drawScanline);
  DisplayController.setResolution(VGA_400x300_60Hz);
  PS2Controller.begin(PS2Preset::KeyboardPort0, KbdMode::GenerateVirtualKeys);
  fgcolor = DisplayController.createRawPixel(RGB222(3, 3, 3)); // white
  bgcolor = DisplayController.createRawPixel(RGB222(1, 1, 1)); // grey
  led1dcolor = DisplayController.createRawPixel(RGB222(1, 0, 0)); // dark red
  led1bcolor = DisplayController.createRawPixel(RGB222(3, 0, 0)); // red
  led2dcolor = DisplayController.createRawPixel(RGB222(0, 1, 0)); // dark green
  led2bcolor = DisplayController.createRawPixel(RGB222(0, 3, 0)); // green
  darkbgcolor = DisplayController.createRawPixel(RGB222(0, 0, 0)); // black
  width  = DisplayController.getScreenWidth();
  height = DisplayController.getScreenHeight();

  // Set CPU bus functions and start it
  m_i8080.setCallbacks(auxPoint, readByte, writeByte, readWord, writeWord, readIO, writeIO); 
  m_i8080.reset();
  for (int i = 0; i < 5; i++) keyboardIn[i]=0xFF;

  // Set function pro Keyboard processing
  PS2Controller.keyboard()->onVirtualKey = [&](VirtualKey * vk, bool keyDown) {
      if (keyDown) {
        procesKeyDown(*vk);
    } else procesKeyUp(*vk);
  };
}

// **************************************************************************************************
// **************************************************************************************************
void loop()
{
  static int numCycles;
  numCycles = 0;
  while(numCycles < 30000) // approx. 30000 cycles per 16.6 milisec (60 Hz VGA)
  {
    for(int i=0; i<8; i++) { digitalWrite(34, HIGH); digitalWrite(34, LOW); } // delay for better audio
    numCycles += m_i8080.step();
    // LOAD and SAVE command is routed to SPIFFS 
    if(m_i8080.getPC() == 0x0B23) {      // instead of the LOAD procedure in ROM, the filling of the memory with the .BAS file is started
      loadFileFlag = true; m_i8080.setPC(0x0BB3);   // We use NAME= input in ROM of JPR1 on the beggining in SAVE
    }
    if(m_i8080.getPC() == 0x0BB3) {      // Here LOAD or SAVE is started
      if(loadFileFlag) { doLoad = true; loadFileFlag = false; } else  doLoad = false;   // We must set load/save flag for SPIFFS operation
    }
    if(m_i8080.getPC() == 0x0BB6) { if(diskFile()) m_i8080.setPC(0x004D); else m_i8080.setPC(0x0BE1); }    // READY or ERROR on tape simulated
  }
  BlinkCnt++;
  if(BlinkCnt >= 30) { BlinkCnt=0; blinkFlag = !blinkFlag; }     // Blink flag manipulation

  // wait for vertical sync
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
