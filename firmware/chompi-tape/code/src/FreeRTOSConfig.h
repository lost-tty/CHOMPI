#pragma once

// Static memory only. TIM4 drives the tick (chompi_main.cpp); SysTick stays libDaisy's.

#define configCPU_CLOCK_HZ 480000000
#define configTICK_RATE_HZ 1000
#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 0
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE 0
#define configMAX_PRIORITIES 6
#define configMINIMAL_STACK_SIZE 128
#define configMAX_TASK_NAME_LEN 8
#define configTICK_TYPE_WIDTH_IN_BITS TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD 1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 1
#define configQUEUE_REGISTRY_SIZE 0
#define configUSE_MUTEXES 0
#define configUSE_RECURSIVE_MUTEXES 0
#define configUSE_COUNTING_SEMAPHORES 0
#define configUSE_TASK_NOTIFICATIONS 1
#define configUSE_TIMERS 0
#define configTIMER_TASK_STACK_DEPTH configMINIMAL_STACK_SIZE // asked for by the kernel's static memory, unused
#define configUSE_CO_ROUTINES 0
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configUSE_MALLOC_FAILED_HOOK 0
#define configCHECK_FOR_STACK_OVERFLOW 0
#define configUSE_TRACE_FACILITY 0
#define configGENERATE_RUN_TIME_STATS 0
#define configUSE_NEWLIB_REENTRANT 0
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 0
#define configKERNEL_PROVIDED_STATIC_MEMORY 1
#define configENABLE_FPU 1
#define configENABLE_MPU 0

// 4 priority bits: interrupts at 0 to 4 never call the kernel, those at 5 to 15 may call the FromISR functions.
#define configPRIO_BITS 4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

#define configASSERT(x)                \
    if(!(x))                           \
    {                                  \
        __asm volatile("cpsid i");     \
        for(;;)                        \
        {                              \
        }                              \
    }

#define INCLUDE_vTaskDelay 1
#define INCLUDE_vTaskDelayUntil 0
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_vTaskDelete 0
#define INCLUDE_uxTaskPriorityGet 0
#define INCLUDE_vTaskPrioritySet 0

// the port's handlers go in the vector table; the tick is ours
#define vPortSVCHandler SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define configOVERRIDE_DEFAULT_TICK_CONFIGURATION 1
