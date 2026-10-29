//
//  pmAMDRyzen.h
//  AMDRyzenCPUPowerManagement
//
//  Created by trulyspinach on 3/27/20.
//

#ifndef pmAMDRyzen_h
#define pmAMDRyzen_h


#include <Headers/mach/mach_types.h>
#include <IOKit/IOLib.h>

#include "Headers/osfmk/i386/pmCPU.h"
#include "Headers/osfmk/i386/cpu_topology.h"

#include "symresolver/kernel_resolver.h"

#include <i386/proc_reg.h>
#include "Headers/pmRyzenSymbolTable.h"

#define MOD_NAME pmARyzen
#define XNU_MAX_CPU 64

// 0.7.6: pmRyzen_stop() 等待 CPU 离开 idle 状态时的最大迭代次数。
// 用来保证停止路径一定会收敛（exit_idle 恒返回 true 时旧代码会死循环）。
#define PMRYZEN_EXIT_IDLE_MAX_SPINS 1000

#define MSR_PSTATE_CTL 0xC0010062
#define MSR_PSTATE_0 0xC0010064

#define EFF_INTERVAL 0.15
#define PSTATE_LIMIT 1
#define PSTATE_LOW_POWER 2
#define PSTATE_STEPDOWN_THRE 0.25
#define PSTATE_STEPUP_THRE 0.45
#define PSTATE_STEPDOWN_TIME 5
#define PSTATE_STEPDOWN_MP_GAIN 1


extern int cpu_number(void);
extern void mp_rendezvous_no_intrs(void (*action_func)(void *), void *arg);

extern x86_lcpu_t *pmRyzen_cpunum_to_lcpu[XNU_MAX_CPU];

extern uint32_t pmRyzen_num_phys;
extern uint32_t pmRyzen_num_logi;

extern uint64_t pmRyzen_exit_idle_c;
extern uint64_t pmRyzen_exit_idle_ipi_c;
extern uint64_t pmRyzen_exit_idle_false_c;

extern uint32_t pmRyzen_hpcpus;

extern uint32_t pmRyzen_pstatelimit;


extern void pmRyzen_wrmsr_safe(void *, uint32_t, uint64_t);
extern uint64_t pmRyzen_rdmsr_safe(void *, uint32_t);
extern pmRyzen_symtable_t pmRyzen_symtable;


typedef struct pmProcessor{
    x86_lcpu_t *lcpu;

    uint64_t stat_exit_idle;
#ifdef PMRYZEN_IDLE_MWAIT
    char pad1[64*64];
#endif
    uint64_t arm_flag;
#ifdef PMRYZEN_IDLE_MWAIT
    char pad2[64*64];
#endif
    uint64_t cpu_awake;
    
    uint64_t last_idle_tsc;
    uint64_t last_start_tsc;
    uint64_t last_idle_length;
    uint64_t last_running_time;
    
    uint64_t eff_timeacc;
    uint64_t eff_idleacc;
    
    float eff_load;
    uint64_t eff_timeaccd;
    uint64_t eff_idleaccd;
    
    uint32_t ll_count;
    uint8_t PState;
    
} pmProcessor_t;

// 0.7.6: 返回 false 表示 PM 注册不可用（符号缺失等）—— 调用方必须让
// start() 失败，不能带着 0 个核继续发布服务（否则用户态会拿 logical/physical
// 做除法，直接把内核带崩）。
boolean_t pmRyzen_init(void*);
void pmRyzen_stop(void);
void pmRyzen_PState_reset(void);
float pmRyzen_avgload_pcpu(uint32_t);

uint64_t pmRyzen_machine_idle(uint64_t);

boolean_t pmRyzen_exit_idle(x86_lcpu_t *);

int pmRyzen_choose_cpu(int,int,int);

pmProcessor_t* pmRyzen_get_processor(uint32_t);

// 0.7.6: 这三个 helper 会在全核屏障内被调用（AMDRyzenCPUPowerManagement.cpp
// 的 init 块与常规 tick）。屏障内一旦越界读或解引用空指针，就是整机死锁
// （NMI 也进不来）。因此这里做完整的边界与空指针检查，失败时返回保守值：
//   - phys_num 失败 → 0（该核的频率数据落在 0 号槽，良性）
//   - primary_in_core 失败 → false（该核被跳过采样，良性）
//   - is_master 失败 → false（master 不刷新 P-state 表，功能降级但不崩）
inline x86_lcpu_t *pmRyzen_get_lcpu(uint32_t cpunum){
    if(cpunum >= XNU_MAX_CPU) return NULL;
    return pmRyzen_cpunum_to_lcpu[cpunum];
}

inline uint32_t pmRyzen_cpu_phys_num(uint32_t cpunum){
    x86_lcpu_t *lcpu = pmRyzen_get_lcpu(cpunum);
    if(lcpu == NULL || lcpu->core == NULL) return 0;
    return lcpu->core->pcore_num;
}

inline boolean_t pmRyzen_cpu_primary_in_core(uint32_t cpunum){
    x86_lcpu_t *lcpu = pmRyzen_get_lcpu(cpunum);
    if(lcpu == NULL || lcpu->core == NULL) return false;
    return lcpu->core->lcpus == lcpu;
}

inline boolean_t pmRyzen_cpu_is_master(uint32_t cpunum){
    x86_lcpu_t *lcpu = pmRyzen_get_lcpu(cpunum);
    if(lcpu == NULL) return false;
    return lcpu->master;
}


#endif /* pmAMDRyzen_h */
