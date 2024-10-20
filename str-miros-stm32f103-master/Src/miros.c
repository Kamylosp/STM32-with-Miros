/****************************************************************************
* MInimal Real-time Operating System (MiROS), GNU-ARM port.
* version 1.26 (matching lesson 26, see https://youtu.be/kLxxXNCrY60)
*
* This software is a teaching aid to illustrate the concepts underlying
* a Real-Time Operating System (RTOS). The main goal of the software is
* simplicity and clear presentation of the concepts, but without dealing
* with various corner cases, portability, or error handling. For these
* reasons, the software is generally NOT intended or recommended for use
* in commercial applications.
*
* Copyright (C) 2018 Miro Samek. All Rights Reserved.
*
* SPDX-License-Identifier: GPL-3.0-or-later
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <https://www.gnu.org/licenses/>.
*
* Git repo:
* https://github.com/QuantumLeaps/MiROS
****************************************************************************/
#include <stdint.h>
#include "miros.h"
#include "qassert.h"
#include "stm32f1xx.h"

Q_DEFINE_THIS_FILE

OSThread * volatile OS_curr; /* pointer to the current thread */
OSThread * volatile OS_next; /* pointer to the next thread to run */

OSThread *OS_thread[32 + 1]; /* array of threads started so far */
uint32_t OS_readySet; /* bitmask of threads that are ready to run */
uint32_t OS_delayedSet; /* bitmask of threads that are delayed */
uint8_t OS_thread_running_index = 0;


#define LOG2(x) (32U - __builtin_clz(x))

OSThread idleThread;
void main_idleThread() {
    while (1) {
        OS_onIdle();
    }
}

void OS_init(void *stkSto, uint32_t stkSize) {
    /* set the PendSV interrupt priority to the lowest level 0xFF */
    *(uint32_t volatile *)0xE000ED20 |= (0xFFU << 16);

    /* start idleThread thread */
    OSThread_start(&idleThread,
                   0U, /* idle thread index */
                   &main_idleThread,
                   stkSto, stkSize);
}


void OS_calculate_next_periodic_task (void){

    uint8_t index_lowest_deadline = 0U;
    uint32_t lowest_deadline;

    uint32_t tasks = OS_delayedSet + OS_readySet;

    while (tasks != 0U){
        OSThread *t = OS_thread[LOG2(tasks)];

        if (t->task_parameters.cost_dinamic > 0){   // Verify if the task have been not all executed
            if (index_lowest_deadline == 0U){           // If It is the first task verifing
                index_lowest_deadline = t->index;
                lowest_deadline = t->task_parameters.deadline_dinamic;

            } else {
                if (t->task_parameters.deadline_dinamic < lowest_deadline){     // If the task have a lowest deadline
                    index_lowest_deadline = t->index;
                    lowest_deadline = t->task_parameters.deadline_dinamic;

                } else if (t->task_parameters.deadline_dinamic == lowest_deadline){
                    if (t->index == OS_thread_running_index){   // If the task is the current task 
                        index_lowest_deadline = t->index;
                        lowest_deadline = t->task_parameters.deadline_dinamic;
                    }
                }
            }
        }
        tasks &= ~(1U << (t->index - 1U)); /* remove from task */
    }

    OS_thread_running_index = index_lowest_deadline;
}




void OS_sched(void) {
    /* choose the next thread to execute... */

    OSThread *next;
    if (OS_thread_running_index == 0U) { /* idle condition? */
        next = OS_thread[0]; /* the idle thread */
    }
    else {
        next = OS_thread[OS_thread_running_index];

        Q_ASSERT(next != (OSThread *)0);
    }

    /* trigger PendSV, if needed */
    if (next != OS_curr) {
        OS_next = next;
        //*(uint32_t volatile *)0xE000ED04 = (1U << 28);
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
        __asm volatile("dsb");
//        __asm volatile("isb");
    }
    /*
     * DSB - whenever a memory access needs to have completed before program execution progresses.
     * ISB - whenever instruction fetches need to explicitly take place after a certain point in the program,
     * for example after memory map updates or after writing code to be executed.
     * (In practice, this means "throw away any prefetched instructions at this point".)
     * */
}

void OS_run(void) {
    /* callback to configure and start interrupts */
    OS_onStartup();

    __disable_irq();
    OS_calculate_next_periodic_task();
    OS_sched();
    __enable_irq();

    /* the following code should never execute */
    Q_ERROR();
}

void OS_tick(void) {
    uint32_t workingSet = OS_delayedSet;
    while (workingSet != 0U) {
        OSThread *t = OS_thread[LOG2(workingSet)];
        uint32_t bit;
        Q_ASSERT((t != (OSThread *)0) && (t->timeout != 0U));

        bit = (1U << (t->index - 1U));
        --t->timeout;
        if (t->timeout == 0U) {
            OS_readySet   |= bit;  /* insert to set */
            OS_delayedSet &= ~bit; /* remove from set */
        }
        workingSet &= ~bit; /* remove from working set */
    }

    /* Update the dinamics parameters os periodics tasks */
    uint32_t tasks = OS_delayedSet + OS_readySet;
    while (tasks != 0U){
        OSThread *t = OS_thread[LOG2(tasks)];
        
        if (t->index == OS_thread_running_index){
            t->task_parameters.cost_dinamic--;
        }

        t->task_parameters.deadline_dinamic--;
        t->task_parameters.period_dinamic--;

        if (t->task_parameters.period_dinamic == 0){
            t->task_parameters.cost_dinamic = t->task_parameters.cost_absolute;
            t->task_parameters.deadline_dinamic = t->task_parameters.deadline_absolute;
            t->task_parameters.period_dinamic = t->task_parameters.period_absolute;
        }

        tasks &= ~(1U << (t->index - 1U)); /* remove from task */
    }
}

void OS_delay(uint32_t ticks) {
    uint32_t bit;
    __asm volatile ("cpsid i");

    /* never call OS_delay from the idleThread */
    Q_REQUIRE(OS_curr != OS_thread[0]);

    OS_curr->timeout = ticks;
    bit = (1U << (OS_curr->index - 1U));
    OS_readySet &= ~bit;
    OS_delayedSet |= bit;
    OS_sched();
    __asm volatile ("cpsie i");
}

/* initialization of the semaphore variable */
void semaphore_init(semaphore_t *p_semaphore, uint32_t start_value){
	if (!p_semaphore){
		__disable_irq();
	}
	p_semaphore->sem_value = start_value;
}

/*  */
void sem_up(semaphore_t *p_semaphore){
	__disable_irq();

	p_semaphore->sem_value++;

	__enable_irq();
}

/*  */
void sem_down(semaphore_t *p_semaphore){
	__disable_irq();

	while (p_semaphore->sem_value == 0){
		OS_delay(1U);
		__disable_irq();
	}

	p_semaphore->sem_value--;
}



void error_indicator_blink() {
	__disable_irq();

	while (1){}
}




void OSThread_start(
    OSThread *me,
    uint8_t index, /* thread index */
    OSThreadHandler threadHandler,
    void *stkSto, uint32_t stkSize)
{
    /* round down the stack top to the 8-byte boundary
    * NOTE: ARM Cortex-M stack grows down from hi -> low memory
    */
    uint32_t *sp = (uint32_t *)((((uint32_t)stkSto + stkSize) / 8) * 8);
    uint32_t *stk_limit;

    /* priority must be in ragne
    * and the priority level must be unused
    */
    Q_REQUIRE((index < Q_DIM(OS_thread))
              && (OS_thread[index] == (OSThread *)0));

    *(--sp) = (1U << 24);  /* xPSR */
    *(--sp) = (uint32_t)threadHandler; /* PC */
    *(--sp) = 0x0000000EU; /* LR  */
    *(--sp) = 0x0000000CU; /* R12 */
    *(--sp) = 0x00000003U; /* R3  */
    *(--sp) = 0x00000002U; /* R2  */
    *(--sp) = 0x00000001U; /* R1  */
    *(--sp) = 0x00000000U; /* R0  */
    /* additionally, fake registers R4-R11 */
    *(--sp) = 0x0000000BU; /* R11 */
    *(--sp) = 0x0000000AU; /* R10 */
    *(--sp) = 0x00000009U; /* R9 */
    *(--sp) = 0x00000008U; /* R8 */
    *(--sp) = 0x00000007U; /* R7 */
    *(--sp) = 0x00000006U; /* R6 */
    *(--sp) = 0x00000005U; /* R5 */
    *(--sp) = 0x00000004U; /* R4 */

    /* save the top of the stack in the thread's attibute */
    me->sp = sp;

    /* round up the bottom of the stack to the 8-byte boundary */
    stk_limit = (uint32_t *)(((((uint32_t)stkSto - 1U) / 8) + 1U) * 8);

    /* pre-fill the unused part of the stack with 0xDEADBEEF */
    for (sp = sp - 1U; sp >= stk_limit; --sp) {
        *sp = 0xDEADBEEFU;
    }

    /* register the thread with the OS */
    OS_thread[index] = me;
    me->index = index;
    /* make the thread ready to run */
    if (index > 0U) {
        OS_readySet |= (1U << (index - 1U));
    }
}

__attribute__ ((naked, optimize("-fno-stack-protector")))
void PendSV_Handler(void) {
__asm volatile (

    /* __disable_irq(); */
    "  CPSID         I                 \n"

    /* if (OS_curr != (OSThread *)0) { */
    "  LDR           r1,=OS_curr       \n"
    "  LDR           r1,[r1,#0x00]     \n"
    "  CBZ           r1,PendSV_restore \n"

    /*     push registers r4-r11 on the stack */
    "  PUSH          {r4-r11}          \n"

    /*     OS_curr->sp = sp; */
    "  LDR           r1,=OS_curr       \n"
    "  LDR           r1,[r1,#0x00]     \n"
    "  STR           sp,[r1,#0x00]     \n"
    /* } */

    "PendSV_restore:                   \n"
    /* sp = OS_next->sp; */
    "  LDR           r1,=OS_next       \n"
    "  LDR           r1,[r1,#0x00]     \n"
    "  LDR           sp,[r1,#0x00]     \n"

    /* OS_curr = OS_next; */
    "  LDR           r1,=OS_next       \n"
    "  LDR           r1,[r1,#0x00]     \n"
    "  LDR           r2,=OS_curr       \n"
    "  STR           r1,[r2,#0x00]     \n"

    /* pop registers r4-r11 */
    "  POP           {r4-r11}          \n"

    /* __enable_irq(); */
    "  CPSIE         I                 \n"

    /* return to the next thread */
    "  BX            lr                \n"
    );
}
