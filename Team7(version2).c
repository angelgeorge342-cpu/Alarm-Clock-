//Team 7 


/* This example accompanies the books
   "Embedded Systems: Real Time Interfacing to ARM Cortex M Microcontrollers",
   ISBN: 978-1463590154, Jonathan Valvano, copyright (c) 2016

   "Embedded Systems: Real-Time Operating Systems for ARM Cortex-M Microcontrollers",
   ISBN: 978-1466468863, Jonathan Valvano, copyright (c) 2016

   "Embedded Systems: Introduction to the MSP432 Microcontroller",
   ISBN: 978-1512185676, Jonathan Valvano, copyright (c) 2016

   "Embedded Systems: Real-Time Interfacing to the MSP432 Microcontroller",
   ISBN: 978-1514676585, Jonathan Valvano, copyright (c) 2016

 Copyright 2016 by Jonathan W. Valvano, valvano@mail.utexas.edu
    You may use, edit, run or distribute this file
    as long as the above copyright notice remains
 THIS SOFTWARE IS PROVIDED "AS IS".  NO WARRANTIES, WHETHER EXPRESS, IMPLIED
 OR STATUTORY, INCLUDING, BUT NOT LIMITED TO, IMPLIED WARRANTIES OF
 MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE APPLY TO THIS SOFTWARE.
 VALVANO SHALL NOT, IN ANY CIRCUMSTANCES, BE LIABLE FOR SPECIAL, INCIDENTAL,
 OR CONSEQUENTIAL DAMAGES, FOR ANY REASON WHATSOEVER.
 For more information about my classes, my research, and my books, see
 http://users.ece.utexas.edu/~valvano/
 */



#include <stdint.h>
#include "../inc/BSP.h"
#include "../inc/Profile.h"
#include "Texas.h"
#include "../inc/CortexM.h"
#include "os.h"

// ===================== CONSTANTS =====================
#define THREADFREQ            1000
#define DISTANCE_THRESHOLD_CM 20   // distance to stop alarm

// ===================== TIME/DATE STRUCTS =====================
typedef struct{
  int hour, min, sec;
} Time_t;

typedef struct{
  int month, day, year, weekday; // weekday: 0=Sun..6=Sat
} Date_t;

// ===================== GLOBALS =====================
volatile Time_t currentTime = {7,29,50};
volatile Time_t alarmTime   = {7,30,0};
volatile Date_t currentDate = {4,18,2026,0};

volatile int alarmActive = 0;      // 0=off, 1=ringing
volatile int mode        = 0;      // 0=normal, 1=set hour, 2=set min

int32_t TimeMutex;
int32_t AlarmSemaphore;

// mailbox data: high 16 bits = distance, low 16 bits = temperature
volatile int LastDistance    = -1;
volatile int LastTemperature = -100;

// ===================== SENSOR STUBS =====================
int32_t BSP_Ultrasonic_GetDistanceCm(void){
  return 100; // far away
}

int32_t BSP_TempSensor_GetCelsius(void){
  return 25;
}

// ===================== ROOM LIGHT USING RGB LED =====================
void RoomLight_Init(void){
  BSP_RGB_Init(0, 0, 0);
}

void RoomLight_On(void){
  BSP_RGB_Set(0, 0, 512); // blue LED
}

void RoomLight_Off(void){
  BSP_RGB_Set(0, 0, 0);
}

// ===================== HELPER: PRINT 2-DIGIT NUMBER =====================
void LCD_Print2(int num, int color){
  if(num < 10){
    BSP_LCD_OutUDec(0,color);
  }
  BSP_LCD_OutUDec(num,color);
}

// ===================== TIME TASK =====================
void TimeTask(void){
  while(1){
    for(int i = 0; i < 100; i++){
      BSP_Delay1ms(10);
    }

    OS_Wait(&TimeMutex);

    currentTime.sec++;
    if(currentTime.sec >= 60){
      currentTime.sec = 0;
      currentTime.min++;
    }
    if(currentTime.min >= 60){
      currentTime.min = 0;
      currentTime.hour++;
    }
    if(currentTime.hour >= 24){
      currentTime.hour = 0;
      currentDate.day++;
      currentDate.weekday = (currentDate.weekday + 1) % 7;

      if(currentDate.day > 30){
        currentDate.day = 1;
        currentDate.month++;
      }
      if(currentDate.month > 12){
        currentDate.month = 1;
        currentDate.year++;
      }
    }

    OS_Signal(&TimeMutex);
  }
}

// ===================== ALARM TASK =====================
void AlarmTask(void){
  while(1){
    for(int i = 0; i < 10; i++){
      BSP_Delay1ms(10);
    }

    OS_Wait(&TimeMutex);

    if(!alarmActive &&
       currentTime.hour == alarmTime.hour &&
       currentTime.min  == alarmTime.min &&
       currentTime.sec  == 0){
      alarmActive = 1;
      OS_Signal(&AlarmSemaphore);
    }

    OS_Signal(&TimeMutex);
  }
}

// ===================== BUZZER + ROOM LIGHT TASK =====================
void BuzzerTask(void){
  while(1){
    OS_Wait(&AlarmSemaphore);

    while(1){
      OS_Wait(&TimeMutex);
      int active = alarmActive;
      OS_Signal(&TimeMutex);

      if(!active){
        BSP_Buzzer_Set(0);
        RoomLight_Off();
        break;
      }

      BSP_Buzzer_Set(512);
      RoomLight_On();
      for(int i = 0; i < 20; i++){
        BSP_Delay1ms(10);
      }

      BSP_Buzzer_Set(0);
      RoomLight_Off();
      for(int i = 0; i < 20; i++){
        BSP_Delay1ms(10);
      }
    }
  }
}

// ===================== PERIODIC SENSOR EVENT (MAILBOX PRODUCER) =====================
void SensorEventTask(void){
  static int count = 0;
  count++;
  if(count >= 100){ // every ~100 ms
    count = 0;

    int d = BSP_Ultrasonic_GetDistanceCm();
    int t = BSP_TempSensor_GetCelsius();

    uint32_t packed = ((d & 0xFFFF) << 16) | (t & 0xFFFF);
    OS_MailBox_Send(packed);
  }
}

// ===================== DISPLAY + BUTTONS + NON-BLOCKING MAILBOX =====================
extern int32_t Send;   // from OS
extern uint32_t Mail;  // from OS

void DisplayTask(void){
  BSP_LCD_Init();
  BSP_LCD_FillScreen(LCD_BLACK);

  BSP_Button1_Init();
  BSP_Button2_Init();

  int last1 = 1;
  int last2 = 1;

  const char* days[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

  while(1){
    // ~100 ms update
    for(int i = 0; i < 10; i++){
      BSP_Delay1ms(10);
    }

    // ===== NON-BLOCKING MAILBOX RECEIVE =====
    if(Send > 0){
      OS_Wait(&Send);
      uint32_t packed = Mail;
      LastDistance    = (int16_t)(packed >> 16);
      LastTemperature = (int16_t)(packed & 0xFFFF);
    }

    // ===== AUTO-STOP ALARM BASED ON DISTANCE =====
    if(alarmActive &&
       LastDistance > 0 &&
       LastDistance <= DISTANCE_THRESHOLD_CM){
      OS_Wait(&TimeMutex);
      alarmActive = 0;
      OS_Signal(&TimeMutex);
    }

    // ===== BUTTONS =====
    int b1 = BSP_Button1_Input();
    int b2 = BSP_Button2_Input();

    // Button1: cycle mode
    if(last1 == 1 && b1 == 0){
      mode++;
      if(mode > 2){
        mode = 0;
      }
    }

    // Button2: on edge
    if(last2 == 1 && b2 == 0){
      OS_Wait(&TimeMutex);

      if(alarmActive){
        // stop alarm regardless of mode
        alarmActive = 0;
      } else if(mode == 1){
        alarmTime.hour = (alarmTime.hour + 1) % 24;
      } else if(mode == 2){
        alarmTime.min = (alarmTime.min + 1) % 60;
      }

      OS_Signal(&TimeMutex);
    }

    last1 = b1;
    last2 = b2;

    // ===== DRAW SCREEN =====
    OS_Wait(&TimeMutex);

    // DATE (centered-ish)
    int startX = 3;
    BSP_LCD_DrawString(startX, 0, (char*)days[currentDate.weekday], LCD_CYAN);
    BSP_LCD_SetCursor(startX + 4, 0);
    LCD_Print2(currentDate.month, LCD_CYAN);
    BSP_LCD_SetCursor(startX + 6, 0);
    BSP_LCD_DrawString(startX + 6, 0, "/", LCD_CYAN);
    BSP_LCD_SetCursor(startX + 7, 0);
    LCD_Print2(currentDate.day, LCD_CYAN);
    BSP_LCD_SetCursor(startX + 9, 0);
    BSP_LCD_DrawString(startX + 9, 0, "/", LCD_CYAN);
    BSP_LCD_SetCursor(startX + 10, 0);
    BSP_LCD_OutUDec4(currentDate.year, LCD_CYAN);

    // STATUS
    if(alarmActive){
      BSP_LCD_DrawString(0,1,"***ALARM***     ", LCD_RED);
    } else if(mode == 1){
      BSP_LCD_DrawString(0,1,"SET ALARM HOUR  ", LCD_YELLOW);
    } else if(mode == 2){
      BSP_LCD_DrawString(0,1,"SET ALARM MIN   ", LCD_YELLOW);
    } else {
      BSP_LCD_DrawString(0,1,"RUNNING         ", LCD_WHITE);
    }

    // TIME
    BSP_LCD_SetCursor(0,3);
    LCD_Print2(currentTime.hour, LCD_GREEN);
    BSP_LCD_SetCursor(3,3);
    BSP_LCD_DrawString(3,3,":", LCD_GREEN);
    BSP_LCD_SetCursor(4,3);
    LCD_Print2(currentTime.min, LCD_GREEN);
    BSP_LCD_SetCursor(7,3);
    BSP_LCD_DrawString(7,3,":", LCD_GREEN);
    BSP_LCD_SetCursor(8,3);
    LCD_Print2(currentTime.sec, LCD_GREEN);

    // ALARM
    BSP_LCD_DrawString(0,5,"Alarm", LCD_ORANGE);
    BSP_LCD_SetCursor(6,5);
    LCD_Print2(alarmTime.hour, LCD_ORANGE);
    BSP_LCD_SetCursor(9,5);
    BSP_LCD_DrawString(9,5,":", LCD_ORANGE);
    BSP_LCD_SetCursor(10,5);
    LCD_Print2(alarmTime.min, LCD_ORANGE);

    // TEMP
    BSP_LCD_DrawString(0,7,"Temp:", LCD_YELLOW);
    BSP_LCD_SetCursor(6,7);
    LCD_Print2(LastTemperature, LCD_YELLOW);
    BSP_LCD_DrawString(9,7,"C ", LCD_YELLOW);

    // DISTANCE
    BSP_LCD_DrawString(0,9,"Dist:", LCD_CYAN);
    BSP_LCD_SetCursor(6,9);
    BSP_LCD_OutUDec(LastDistance, LCD_CYAN);
    BSP_LCD_DrawString(10,9,"cm ", LCD_CYAN);

    // DEBUG BUTTON STATES
    BSP_LCD_DrawString(0,11, b1 == 0 ? "B1:PRESSED " : "B1:-----   ", LCD_GREEN);
    BSP_LCD_DrawString(0,12, b2 == 0 ? "B2:PRESSED " : "B2:-----   ", LCD_GREEN);

    OS_Signal(&TimeMutex);
  }
}

// ===================== MAIN =====================
int main(void){
  OS_Init();
  Profile_Init();

  BSP_LCD_Init();
  BSP_Buzzer_Init(0);
  RoomLight_Init();

  OS_InitSemaphore(&AlarmSemaphore, 0);
  OS_InitSemaphore(&TimeMutex, 1);
  OS_MailBox_Init();

  OS_AddThreads(&TimeTask, &AlarmTask, &DisplayTask, &BuzzerTask);
  OS_AddPeriodicEventThreads(&SensorEventTask, 1, 0, 0);

  TExaS_Init(GRADER, 1000);

  OS_Launch(BSP_Clock_GetFreq()/THREADFREQ);
  return 0;
}
