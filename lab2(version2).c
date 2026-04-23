// Team_7.c
// Smart Alarm Clock System

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "os.h"
#include "../inc/BSP.h"
#include "../inc/tm4c123gh6pm.h"

//  CortexM helpers — declared in os.c / CortexM.h

void DisableInterrupts(void);
void EnableInterrupts(void);
long StartCritical(void);
void EndCritical(long sr);


//  SCREEN STATES

typedef enum {
    SCREEN_HOME = 0,      // current time / date / alarm menu entry
    SCREEN_ALARM_SET,     // set alarm  Hour : Min  AM/PM
    SCREEN_ALARM_FIRING,  // alarm ringing  ->  Stop / Snooze
    SCREEN_SNOOZE,        // snooze countdown
    SCREEN_TIME_SET       // set current time & date
} ScreenState_t;


//  REAL-TIME CLOCK  

typedef struct {
    uint8_t  hours;    // 0-23 (24-hr internal)
    uint8_t  minutes;
    uint8_t  seconds;
    uint8_t  month;
    uint8_t  day;
    uint16_t year;
} RTC_t;


//  SHARED GLOBALS

// Start in Central Time: 8:30 AM, 4/27/2026
volatile RTC_t         g_Time        = {8, 30, 0, 4, 27, 2026};
volatile RTC_t         g_AlarmTime   = {0, 0, 0, 0, 0, 0};

volatile ScreenState_t g_Screen       = SCREEN_HOME;
volatile uint8_t       g_AlarmEnabled = 0;  // 1 = alarm armed
volatile uint8_t       g_AlarmFiring  = 0;  // 1 = alarm currently ringing
volatile uint8_t       g_BuzzerOn     = 0;  // 1 = buzzer should sound
volatile uint8_t       g_DisplayDirty = 1;  // 1 = LCD needs refresh
volatile uint32_t      g_SnoozeRemain = 0;  // seconds left in snooze
volatile uint8_t g_LastStoppedByMovement = 0;  // 1 = last alarm auto-stopped by movement

// Joystick values written by PeriodicTask2, read by JoystickTask
volatile uint16_t      g_JoyX  = 512;    // 0-1023
volatile uint16_t      g_JoyY  = 512;
volatile uint8_t       g_JoySel = 0x10;  // 0x10=not pressed, 0=pressed

// UI cursor (meaning depends on current screen)
volatile uint8_t       g_Cursor = 0;

// Alarm-set UI working copy
volatile uint8_t       g_SetCol   = 0;   // which field is active
volatile uint8_t       g_SetHour  = 12;  // 1-12
volatile uint8_t       g_SetMin   = 0;   // 0-59
volatile uint8_t       g_SetAMPM  = 0;   // 0=AM, 1=PM

// Time/Date-set UI working copy (for SCREEN_TIME_SET)
volatile uint8_t       g_SetMonth = 4;   // 1-12
volatile uint8_t       g_SetDay   = 27;  // 1-31
volatile uint16_t      g_SetYear  = 2026;

//  SEMAPHORES  

int32_t g_SemaAlarm;    // signaled when alarm fires / snooze expires
int32_t g_SemaStop;     // signaled when user picks Stop
int32_t g_SemaSnooze;   // signaled when user picks Snooze


volatile uint32_t g_TickMs = 0;

// Spin-wait for ms milliseconds.
// SysTick preempts every 1 ms so other threads still run.
static void SleepMs(uint32_t ms) {
    uint32_t start = g_TickMs;
    while ((g_TickMs - start) < ms) {}
}

// Convert 24-hr to 12-hr (1-12)
static uint8_t To12Hr(uint8_t h24) {
    uint8_t h = h24 % 12;
    return (h == 0) ? 12 : h;
}

// Return 1 if hour is AM
static uint8_t IsAM(uint8_t h24) { return (h24 < 12) ? 1 : 0; }

// Joystick ADC thresholds (0-1023 scale)
#define JOY_HI   800u   // pushed toward high end  (up / right)
#define JOY_LO   200u   // pushed toward low end   (down / left)
#define JOY_DEB  250u   // minimum ms between actions


void PeriodicTask1_1ms(void) {
    g_TickMs++;

    // Decrement snooze countdown once per second
    static uint32_t secAcc = 0;
    if (!g_AlarmFiring && g_SnoozeRemain > 0) {
        secAcc++;
        if (secAcc >= 1000u) {
            secAcc = 0;
            g_SnoozeRemain--;
            g_DisplayDirty = 1;

            if (g_SnoozeRemain == 0) {
                // Snooze expired — re-fire alarm
                g_AlarmFiring  = 1;
                g_BuzzerOn     = 1;
                g_Screen       = SCREEN_ALARM_FIRING;
                g_Cursor       = 0;
                OS_Signal(&g_SemaAlarm);
            }
        }
    }
}


//  called every 100 ms by Scheduler()
void PeriodicTask2_100ms(void) {
    uint16_t x, y;
    uint8_t  sel;
    BSP_Joystick_Input(&x, &y, &sel);
    g_JoyX   = x;
    g_JoyY   = y;
    g_JoySel = sel;
}

// ClockTask
//  Increments time/date and checks alarm
void ClockTask(void) {
    // Days per month (non-leap, index 1-12)
    static const uint8_t dpm[13] = {0,31,28,31,30,31,30,31,31,30,31,30,31};

    while (1) {
        SleepMs(1000); // spin-wait 1 second

        long sr = StartCritical();

        g_Time.seconds++;
        if (g_Time.seconds >= 60) {
            g_Time.seconds = 0;
            g_Time.minutes++;
        }
        if (g_Time.minutes >= 60) {
            g_Time.minutes = 0;
            g_Time.hours++;
        }
        if (g_Time.hours >= 24) {
            g_Time.hours = 0;
            g_Time.day++;
            if (g_Time.day > dpm[g_Time.month]) {
                g_Time.day = 1;
                g_Time.month++;
                if (g_Time.month > 12) {
                    g_Time.month = 1;
                    g_Time.year++;
                }
            }
        }

        // Check alarm match (armed, not already firing, seconds=0)
        uint8_t fireNow = 0;
        if (g_AlarmEnabled && !g_AlarmFiring) {
            if (g_Time.hours   == g_AlarmTime.hours  &&
                g_Time.minutes == g_AlarmTime.minutes &&
                g_Time.seconds == 0) {
                fireNow = 1;
            }
        }

        EndCritical(sr);

        if (fireNow) {
            g_AlarmFiring  = 1;
            g_BuzzerOn     = 1;
            g_Screen       = SCREEN_ALARM_FIRING;
            g_Cursor       = 0;
            OS_Signal(&g_SemaAlarm); // wake AlarmBuzzerTask
        }

        g_DisplayDirty = 1; // refresh display every second (but no full clear)
    }
}


//  THREAD 1 — DisplayTask
//  Redraws LCD whenever g_DisplayDirty is set.

void DisplayTask(void) {
    char buf[32];
    ScreenState_t lastScreen = 0xFF; // invalid initial

    while (1) {
        // Spin until a refresh is needed
        while (g_DisplayDirty == 0) {}
        g_DisplayDirty = 0;

        // Snapshot shared state (single-word reads are atomic)
        uint8_t  h   = g_Time.hours;
        uint8_t  mn  = g_Time.minutes;
        uint8_t  sc  = g_Time.seconds;
        uint8_t  mo  = g_Time.month;
        uint8_t  dy  = g_Time.day;
        uint16_t yr  = g_Time.year;
        uint8_t  ah  = g_AlarmTime.hours;
        uint8_t  amn = g_AlarmTime.minutes;

        uint8_t dispH  = To12Hr(h);
        uint8_t isPM   = IsAM(h) ? 0 : 1;
				
					 // If screen changed, clear once and redraw static layout
        if (g_Screen != lastScreen) {
            BSP_LCD_FillScreen(LCD_BLACK);
            lastScreen = g_Screen;
        }


        switch (g_Screen) {

            // ------------------------------------------------
            //  HOME
            //  Row 0:  HH:MM AM/PM
            //  Row 1:  M/DD/YY
            //  Row 2:  [>] Alarm  HH:MM  (cursor==1)
            //  Row 3:  [>] Set Time & Date (cursor==2)
            //  Row 5:     :SS     (live seconds)
            // ------------------------------------------------
            case SCREEN_HOME: {
							
							
								// -------- PROJECT TITLE (CENTERED) --------
								char title[] = "TEAM 7 RTOS ALARM";
								int colTitle = (21 - strlen(title)) / 2;
								BSP_LCD_DrawString(colTitle, 0, title, LCD_MAGENTA);
								// -------- BLANK SPACER ROW --------
								// (Row 1 intentionally left empty)
                // Time (centered)
                snprintf(buf, sizeof(buf), "%2u:%02u %s",
                         (unsigned)dispH, (unsigned)mn,
                         isPM ? "PM" : "AM");
								int colTime = (21 - strlen(buf)) / 2;
                BSP_LCD_DrawString(colTime, 2, buf, LCD_WHITE);

                // Date (centered)
                snprintf(buf, sizeof(buf), "%u/%u/%02u",
                         (unsigned)mo, (unsigned)dy,
                         (unsigned)(yr % 100u));
								int colDate = (21 - strlen(buf)) / 2;
                BSP_LCD_DrawString(colDate, 3, buf, LCD_WHITE);

                // Alarm row
                uint16_t aColor = (g_Cursor == 1) ? LCD_YELLOW : LCD_BLUE;
                if (g_Cursor == 1) {
                    BSP_LCD_DrawString(0, 5, ">", LCD_YELLOW);
                } else {
                    BSP_LCD_DrawString(0, 5, " ", LCD_BLACK);
                }
                if (g_AlarmEnabled) {
                    snprintf(buf, sizeof(buf), "Alarm %2u:%02u%s",
                             (unsigned)To12Hr(ah), (unsigned)amn,
                             IsAM(ah) ? "AM" : "PM");
                } else {
                    snprintf(buf, sizeof(buf), "Alarm --:--");
                }
                BSP_LCD_DrawString(1, 5, buf, aColor);

                // Set Time & Date row
                uint16_t tColor = (g_Cursor == 2) ? LCD_YELLOW : LCD_CYAN;
                if (g_Cursor == 2) {
                    BSP_LCD_DrawString(0, 6, ">", LCD_YELLOW);
                } else {
                    BSP_LCD_DrawString(0, 6, " ", LCD_BLACK);
                }
                BSP_LCD_DrawString(1, 6, "Set Time & Date", tColor);

                // Live seconds indicator
								// Seconds label
								BSP_LCD_DrawString(0, 7, "Seconds:", LCD_GREEN);

								// Seconds value (00–59)
								snprintf(buf, sizeof(buf), "%02u", (unsigned)sc);
								BSP_LCD_DrawString(9, 7, buf, LCD_GREEN);
                
						
								// Status line for last alarm cause
								if (g_LastStoppedByMovement) {
									BSP_LCD_DrawString(0, 8, "Last alarm: ", LCD_ORANGE);
									BSP_LCD_DrawString(0, 9, "stopped with movement", LCD_ORANGE);
							} else {
								BSP_LCD_DrawString(0, 8, "                     ", LCD_BLACK);
								BSP_LCD_DrawString(0, 9, "                     ", LCD_BLACK);
							}

								break;
						}



            // ------------------------------------------------
            //  ALARM SET
            //  Row 0:  "Set Alarm"
            //  Row 2:  " HH : MM  AP "   active field = YELLOW
            //  Row 4:  "< > field"
            //  Row 5:  "^ v value"
            //  Row 6:  "Btn2=Done"
            // ------------------------------------------------
            case SCREEN_ALARM_SET: {
                BSP_LCD_DrawString(0, 0, "Set Alarm       ", LCD_CYAN);

                snprintf(buf, sizeof(buf), "%02u", (unsigned)g_SetHour);
                BSP_LCD_DrawString(1, 2, buf,
                    (g_SetCol == 0) ? LCD_YELLOW : LCD_WHITE);

                BSP_LCD_DrawString(3, 2, ":", LCD_WHITE);

                snprintf(buf, sizeof(buf), "%02u", (unsigned)g_SetMin);
                BSP_LCD_DrawString(4, 2, buf,
                    (g_SetCol == 1) ? LCD_YELLOW : LCD_WHITE);

                BSP_LCD_DrawString(7, 2,
                    (g_SetAMPM == 0) ? "AM" : "PM",
                    (g_SetCol == 2) ? LCD_YELLOW : LCD_WHITE);

                BSP_LCD_DrawString(0, 4, "< > field ",  LCD_GREEN);
                BSP_LCD_DrawString(0, 5, "^ v value ",  LCD_GREEN);
                BSP_LCD_DrawString(0, 6, "Btn2=Done",  LCD_MAGENTA);
                break;
            }

            // ------------------------------------------------
            //  TIME & DATE SET
            //  Row 0:  "Set Time & Date"
            //  Row 2:  " HH : MM  AP "   (col 0-2)
            //  Row 3:  " MM / DD / YYYY" (col 3-5)
            //  Row 5:  "< > field"
            //  Row 6:  "^ v value"
            //  Row 7:  "Btn2=Done"
            // ------------------------------------------------
            case SCREEN_TIME_SET: {
                BSP_LCD_DrawString(0, 0, "Set Time & Date ", LCD_CYAN);

                // Time row
                snprintf(buf, sizeof(buf), "%02u", (unsigned)g_SetHour);
                BSP_LCD_DrawString(1, 2, buf,
                    (g_SetCol == 0) ? LCD_YELLOW : LCD_WHITE);

                BSP_LCD_DrawString(3, 2, ":", LCD_WHITE);

                snprintf(buf, sizeof(buf), "%02u", (unsigned)g_SetMin);
                BSP_LCD_DrawString(4, 2, buf,
                    (g_SetCol == 1) ? LCD_YELLOW : LCD_WHITE);

                BSP_LCD_DrawString(7, 2,
                    (g_SetAMPM == 0) ? "AM" : "PM",
                    (g_SetCol == 2) ? LCD_YELLOW : LCD_WHITE);

                // Date row
                snprintf(buf, sizeof(buf), "%02u", (unsigned)g_SetMonth);
                BSP_LCD_DrawString(1, 3, buf,
                    (g_SetCol == 3) ? LCD_YELLOW : LCD_WHITE);

                BSP_LCD_DrawString(3, 3, "/", LCD_WHITE);

                snprintf(buf, sizeof(buf), "%02u", (unsigned)g_SetDay);
                BSP_LCD_DrawString(4, 3, buf,
                    (g_SetCol == 4) ? LCD_YELLOW : LCD_WHITE);

                BSP_LCD_DrawString(6, 3, "/", LCD_WHITE);

                snprintf(buf, sizeof(buf), "%04u", (unsigned)g_SetYear);
                BSP_LCD_DrawString(7, 3, buf,
                    (g_SetCol == 5) ? LCD_YELLOW : LCD_WHITE);

                BSP_LCD_DrawString(0, 5, "< > field ",  LCD_GREEN);
                BSP_LCD_DrawString(0, 6, "^ v value ",  LCD_GREEN);
                BSP_LCD_DrawString(0, 7, "Btn2=Done",  LCD_MAGENTA);
                break;
            }

            // ------------------------------------------------
            //  ALARM FIRING
            //  Row 0:  "!!! ALARM !!!"
            //  Row 2:  "> Stop"    (yellow if cursor=0)
            //  Row 3:  "> Snooze"  (yellow if cursor=1)
            //  Row 5:  navigation hint
            // ------------------------------------------------
            case SCREEN_ALARM_FIRING: {
                // For alarm, we allow a full clear to emphasize flashing
                BSP_LCD_FillScreen(LCD_BLACK);
                BSP_LCD_DrawString(0, 0, "!!! ALARM !!!", LCD_RED);

                BSP_LCD_DrawString(0, 2,
                    (g_Cursor == 0) ? ">Stop  " : " Stop  ",
                    (g_Cursor == 0) ? LCD_YELLOW : LCD_WHITE);

                BSP_LCD_DrawString(0, 3,
                    (g_Cursor == 1) ? ">Snooze" : " Snooze",
                    (g_Cursor == 1) ? LCD_YELLOW : LCD_WHITE);

                BSP_LCD_DrawString(0, 5, "^v=sel Sbt=ok", LCD_GREEN);
                break;
            }

            // ------------------------------------------------
            //  SNOOZE COUNTDOWN
            //  Row 0:  "Snoozing..."
            //  Row 2:  "MM:SS left"
            //  Row 5:  current time (small)
            // ------------------------------------------------
            case SCREEN_SNOOZE: {
                BSP_LCD_DrawString(0, 0, "Snoozing...    ", LCD_CYAN);

                uint32_t rem  = g_SnoozeRemain;
                uint32_t mRem = rem / 60u;
                uint32_t sRem = rem % 60u;
                snprintf(buf, sizeof(buf), "%02u:%02u left",
                         (unsigned)mRem, (unsigned)sRem);
                BSP_LCD_DrawString(1, 2, buf, LCD_YELLOW);

                snprintf(buf, sizeof(buf), "%2u:%02u %s",
                         (unsigned)dispH, (unsigned)mn,
                         isPM ? "PM" : "AM");
                BSP_LCD_DrawString(0, 5, buf, LCD_WHITE);
                break;
            }
        }
    }
}

//  THREAD 2 — JoystickTask
//  Reads g_JoyX/Y/Sel (set by PeriodicTask2 every 100 ms) and

// Bit-band alias for PD7 (Button2) — matches BSP.c BUTTON2 definition
#define BUTTON2_VAL  (*((volatile uint32_t *)0x40007200u))

void JoystickTask(void) {
    uint32_t lastAct = 0;      // g_TickMs of last accepted action
    uint8_t  prevSel  = 0x10;  // previous joystick select state
    uint8_t  prevBtn2 = 0x80;  // previous Button2 state (0=pressed)

    while (1) {
        SleepMs(50); // poll at ~20 Hz

        uint16_t jx   = g_JoyX;
        uint16_t jy   = g_JoyY;
        uint8_t  sel  = g_JoySel;
        uint8_t  btn2 = (uint8_t)BUTTON2_VAL; // 0 or 0x80

        uint32_t now    = g_TickMs;
        bool     canAct = ((now - lastAct) >= JOY_DEB);


        //  HOME

        if (g_Screen == SCREEN_HOME) {

            // DOWN: move cursor 0->1->2
            if (canAct && jy < JOY_LO) {
                if (g_Cursor < 2) {
                    g_Cursor++;
                    g_DisplayDirty = 1;
                    lastAct = now;
                }
            }
            // UP: move cursor 2->1->0
            if (canAct && jy > JOY_HI) {
                if (g_Cursor > 0) {
                    g_Cursor--;
                    g_DisplayDirty = 1;
                    lastAct = now;
                }
            }

            // Joystick click (falling edge)
            if ((sel == 0) && (prevSel != 0) && canAct) {
                lastAct = now;

                // Enter Alarm Set
                if (g_Cursor == 1) {
                    if (g_AlarmEnabled) {
                        g_SetHour = To12Hr(g_AlarmTime.hours);
                        g_SetMin  = g_AlarmTime.minutes;
                        g_SetAMPM = IsAM(g_AlarmTime.hours) ? 0 : 1;
                    } else {
                        g_SetHour = 12;
                        g_SetMin  = 0;
                        g_SetAMPM = 0;
                    }
                    g_SetCol       = 0;
                    g_Screen       = SCREEN_ALARM_SET;
                    g_DisplayDirty = 1;
                }
                // Enter Time & Date Set
                else if (g_Cursor == 2) {
                    g_SetHour  = To12Hr(g_Time.hours);
                    g_SetMin   = g_Time.minutes;
                    g_SetAMPM  = IsAM(g_Time.hours) ? 0 : 1;
                    g_SetMonth = g_Time.month;
                    g_SetDay   = g_Time.day;
                    g_SetYear  = g_Time.year;
                    g_SetCol   = 0;
                    g_Screen   = SCREEN_TIME_SET;
                    g_DisplayDirty = 1;
                }
            }
        }


        //  ALARM SET

        else if (g_Screen == SCREEN_ALARM_SET) {

            if (canAct) {
                bool moved = false;

                if (jx < JOY_LO) {                          // LEFT ? prev field
                    if (g_SetCol > 0) g_SetCol--;
                    moved = true;
                } else if (jx > JOY_HI) {                   // RIGHT ? next field
                    if (g_SetCol < 2) g_SetCol++;
                    moved = true;
                }

                if (jy > JOY_HI) {                          // UP ? increment
                    switch (g_SetCol) {
                        case 0: g_SetHour = (g_SetHour >= 12) ? 1  : g_SetHour + 1; break;
                        case 1: g_SetMin  = (g_SetMin  >= 59) ? 0  : g_SetMin  + 1; break;
                        case 2: g_SetAMPM ^= 1; break;
                    }
                    moved = true;
                } else if (jy < JOY_LO) {                   // DOWN ? decrement
                    switch (g_SetCol) {
                        case 0: g_SetHour = (g_SetHour <= 1)  ? 12 : g_SetHour - 1; break;
                        case 1: g_SetMin  = (g_SetMin  == 0)  ? 59 : g_SetMin  - 1; break;
                        case 2: g_SetAMPM ^= 1; break;
                    }
                    moved = true;
                }

                if (moved) { lastAct = now; g_DisplayDirty = 1; }
            }

            // Button2 falling edge ? DONE: save alarm and return to HOME
            if ((btn2 == 0) && (prevBtn2 != 0) && canAct) {
                lastAct = now;
                long sr = StartCritical();
                uint8_t h24 = g_SetHour % 12;       // 12?0, 1..11 ? 1..11
                if (g_SetAMPM == 1) h24 += 12;      // PM
                g_AlarmTime.hours   = h24;
                g_AlarmTime.minutes = g_SetMin;
                g_AlarmTime.seconds = 0;
                EndCritical(sr);

                g_AlarmEnabled = 1;
                g_Cursor       = 0;
                g_Screen       = SCREEN_HOME;
                g_DisplayDirty = 1;
            }
        }


        //  TIME & DATE SET

        else if (g_Screen == SCREEN_TIME_SET) {

            if (canAct) {
                bool moved = false;

                // LEFT/RIGHT: move between 6 fields (0..5)
                if (jx < JOY_LO) {
                    if (g_SetCol > 0) g_SetCol--;
                    moved = true;
                } else if (jx > JOY_HI) {
                    if (g_SetCol < 5) g_SetCol++;
                    moved = true;
                }

                // UP/DOWN: change value of active field
                if (jy > JOY_HI) { // UP = increment
                    switch (g_SetCol) {
                        case 0: // hour
                            g_SetHour = (g_SetHour >= 12) ? 1 : g_SetHour + 1;
                            break;
                        case 1: // minute
                            g_SetMin = (g_SetMin >= 59) ? 0 : g_SetMin + 1;
                            break;
                        case 2: // AM/PM
                            g_SetAMPM ^= 1;
                            break;
                        case 3: // month
                            g_SetMonth = (g_SetMonth >= 12) ? 1 : g_SetMonth + 1;
                            break;
                        case 4: // day
                            g_SetDay = (g_SetDay >= 31) ? 1 : g_SetDay + 1;
                            break;
                        case 5: // year
                            g_SetYear++;
                            break;
                    }
                    moved = true;
                } else if (jy < JOY_LO) { // DOWN = decrement
                    switch (g_SetCol) {
                        case 0: // hour
                            g_SetHour = (g_SetHour <= 1) ? 12 : g_SetHour - 1;
                            break;
                        case 1: // minute
                            g_SetMin = (g_SetMin == 0) ? 59 : g_SetMin - 1;
                            break;
                        case 2: // AM/PM
                            g_SetAMPM ^= 1;
                            break;
                        case 3: // month
                            g_SetMonth = (g_SetMonth <= 1) ? 12 : g_SetMonth - 1;
                            break;
                        case 4: // day
                            g_SetDay = (g_SetDay <= 1) ? 31 : g_SetDay - 1;
                            break;
                        case 5: // year
                            if (g_SetYear > 2000) g_SetYear--;
                            break;
                    }
                    moved = true;
                }

                if (moved) {
                    lastAct = now;
                    g_DisplayDirty = 1;
                }
            }

            // Button2 falling edge ? DONE: save time/date and return to HOME
            if ((btn2 == 0) && (prevBtn2 != 0) && canAct) {
                lastAct = now;

                uint8_t h24 = g_SetHour % 12;
                if (g_SetAMPM == 1) h24 += 12;

                long sr = StartCritical();
                g_Time.hours   = h24;
                g_Time.minutes = g_SetMin;
                g_Time.seconds = 0;
                g_Time.month   = g_SetMonth;
                g_Time.day     = g_SetDay;
                g_Time.year    = g_SetYear;
                EndCritical(sr);

                g_Screen       = SCREEN_HOME;
                g_Cursor       = 0;
                g_DisplayDirty = 1;
            }
        }


        //  ALARM FIRING

        else if (g_Screen == SCREEN_ALARM_FIRING) {

            if (canAct && jy > JOY_HI) {   // UP ? highlight Stop
                g_Cursor = 0; g_DisplayDirty = 1; lastAct = now;
            }
            if (canAct && jy < JOY_LO) {   // DOWN ? highlight Snooze
                g_Cursor = 1; g_DisplayDirty = 1; lastAct = now;
            }

            // Joystick click (falling edge) ? execute selection
            if ((sel == 0) && (prevSel != 0) && canAct) {
                lastAct = now;
                if (g_Cursor == 0) {
                    OS_Signal(&g_SemaStop);
                } else {
                    OS_Signal(&g_SemaSnooze);
                }
            }
        }

        // SNOOZE screen: no user interaction required (just wait)

        prevSel  = sel;
        prevBtn2 = btn2;
    }
}

//  THREAD 3 — AlarmBuzzerTask
//  Waits on g_SemaAlarm, drives buzzer, handles Stop/Snooze.
	void AlarmBuzzerTask(void) {
    #define SNOOZE_SECS 300u
    #define BEEP_ON_MS  400u
    #define BEEP_OFF_MS 400u
    #define MOVE_THRESHOLD 60  // movement sensitivity
		// Simple unsigned absolute difference
    #define ABS_DIFF(a,b) (( (a) > (b) ) ? ((a)-(b)) : ((b)-(a)))
    uint16_t prevAx = 0, prevAy = 0, prevAz = 0;
		

    while (1) {
        // Wait until alarm is triggered
        OS_Wait(&g_SemaAlarm);

        g_Screen       = SCREEN_ALARM_FIRING;
        g_Cursor       = 0;
        g_AlarmFiring  = 1;
        g_BuzzerOn     = 1;
        g_DisplayDirty = 1;
			 BSP_Accelerometer_Input(&prevAx, &prevAy, &prevAz);
				

        bool handled = false;

        while (!handled) {

            // BEEP ON + LED ON 
            BSP_Buzzer_Set(512);
            BSP_RGB_Set(0, 0, 1023);   // BLUE LED ON
            SleepMs(BEEP_ON_MS);

            // BEEP OFF + LED OFF 
            BSP_Buzzer_Set(0);
            BSP_RGB_Set(0, 0, 0);      // LED OFF
            SleepMs(BEEP_OFF_MS);
						
					 // Allow DisplayTask to refresh alarm screen
            g_DisplayDirty = 1;
					
            // MOVEMENT CHECK (accelerometer)
            uint16_t ax, ay, az;
            BSP_Accelerometer_Input(&ax, &ay, &az);

            if (ABS_DIFF(ax, prevAx) > MOVE_THRESHOLD ||
                ABS_DIFF(ay, prevAy) > MOVE_THRESHOLD ||
                ABS_DIFF(az, prevAz) > MOVE_THRESHOLD) {

                // Auto-stop by movement
                BSP_Buzzer_Set(0);
                BSP_RGB_Set(0, 0, 0);
                g_BuzzerOn     = 0;
                g_AlarmFiring  = 0;
                g_Cursor       = 0;

                g_LastStoppedByMovement = 1;

                BSP_LCD_FillScreen(LCD_BLACK);
                BSP_LCD_DrawString(0, 0, "Alarm Stopped",      LCD_ORANGE);
                BSP_LCD_DrawString(0, 1, "Movement Detected",  LCD_ORANGE );
                SleepMs(1500);

                g_Screen       = SCREEN_HOME;
                g_DisplayDirty = 1;
                handled        = true;
                continue;
            }

           

            // STOP (joystick Stop)
            if (g_SemaStop > 0) {
                OS_Wait(&g_SemaStop);

                BSP_Buzzer_Set(0);
                BSP_RGB_Set(0, 0, 0);
                g_BuzzerOn     = 0;
                g_AlarmFiring  = 0;
                g_Cursor       = 0;

                g_LastStoppedByMovement = 0;

                g_Screen       = SCREEN_HOME;
                g_DisplayDirty = 1;
                handled        = true;
            }

            // SNOOZE 
            if (!handled && g_SemaSnooze > 0) {
                OS_Wait(&g_SemaSnooze);

                BSP_Buzzer_Set(0);
                BSP_RGB_Set(0, 0, 0);
                g_BuzzerOn     = 0;
                g_AlarmFiring  = 0;
                g_Cursor       = 0;

                g_LastStoppedByMovement = 0;

                g_SnoozeRemain = SNOOZE_SECS;
                g_Screen       = SCREEN_SNOOZE;
                g_DisplayDirty = 1;
                handled        = true;
            }
        }
    }
}

//Main
int main(void) {

    // 1. Initialize RTOS (sets 80 MHz clock, disables interrupts)
    OS_Init();

    // 2. Initialize hardware peripherals
    BSP_LCD_Init();
    BSP_LCD_FillScreen(LCD_BLACK);

    // Joystick: PE4 select, PB5 X-axis, PD3 Y-axis
    BSP_Joystick_Init();

    // Button2: PD7 (used as "Done" in set screens)
    BSP_Button2_Init();

    // Buzzer: PF2 Timer1A PWM — initialize silent
    BSP_Buzzer_Init(0);
		// RGB LED (start off)
		BSP_RGB_Init(0, 0, 0);
		//Accelerometer
		BSP_Accelerometer_Init();
    // Temperature sensor on I2C (for future extension)
    BSP_TempSensor_Init();
		


    // 3. Initialize semaphores (all start blocked = 0)
    OS_InitSemaphore(&g_SemaAlarm,  0);
    OS_InitSemaphore(&g_SemaStop,   0);
    OS_InitSemaphore(&g_SemaSnooze, 0);

    // 4. Initialize mailbox (available for temperature data if extended)
    OS_MailBox_Init();

    // 5. Add exactly 4 foreground round-robin threads
    OS_AddThreads(&ClockTask,
                  &DisplayTask,
                  &JoystickTask,
                  &AlarmBuzzerTask);

    // 6. Add 2 background periodic event threads
    //    Both use period=1 (every SysTick = 1 ms).
    //    Scheduler() in os.c calls PeriodicTask2 every 100 invocations
    //    of PeriodicTask1, giving PeriodicTask2 an effective 100 ms period.
    OS_AddPeriodicEventThreads(&PeriodicTask1_1ms,   1,
                               &PeriodicTask2_100ms, 1);

    // 7. Launch — does not return
    //    Slice = 80 MHz / 1000 = 80,000 cycles = 1 ms per thread
    OS_Launch(BSP_Clock_GetFreq() / 1000u);

    while (1) {}   // unreachable
}
