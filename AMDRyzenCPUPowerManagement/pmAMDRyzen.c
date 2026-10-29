//
//  pmAMDRyzen.c
//  AMDRyzenCPUPowerManagement
//
//  Created by trulyspinach on 3/27/20.
//

#include "pmAMDRyzen.h"



x86_lcpu_t *pmRyzen_cpunum_to_lcpu[XNU_MAX_CPU];

void *pmRyzen_io_service_handle;

pmProcessor_t pmRyzen_cpus[XNU_MAX_CPU];
uint64_t pmRyzen_tsc_freq;
uint64_t pmRyzen_effective_timetsc;

uint32_t pmRyzen_num_phys;
uint32_t pmRyzen_num_logi;

uint64_t pmRyzen_exit_idle_c;
uint64_t pmRyzen_exit_idle_ipi_c;
uint64_t pmRyzen_exit_idle_false_c;

uint64_t pmRyzen_p_sdtsc;
uint64_t pmRyzen_p_sutsc;

uint32_t pmRyzen_hpcpus = 0;
uint32_t pmRyzen_pstatelimit;

void(*pmRyzen_pmUnRegister)(pmDispatch_t*) = 0;
void(*pmRyzen_cpu_NMI)(int) = 0;
void(*pmRyzen_NMI_enabled)(boolean_t) = 0;
void(*pmRyzen_cpu_IPI)(int) = 0;


//kern_return_t       (*pmCPUStateInit)(void);
//void                (*cstateInit)(void);
//uint64_t            (*MachineIdle)(uint64_t maxIdleDuration);
//uint64_t            (*GetDeadline)(x86_lcpu_t *lcpu);
//uint64_t            (*SetDeadline)(x86_lcpu_t *lcpu, uint64_t);
//void                (*Deadline)(x86_lcpu_t *lcpu);
//boolean_t           (*exitIdle)(x86_lcpu_t *lcpu);
//void                (*markCPURunning)(x86_lcpu_t *lcpu);
//int                 (*pmCPUControl)(uint32_t cmd, void *datap);
//void                (*pmCPUHalt)(void);
//uint64_t            (*getMaxSnoop)(void);
//void                (*setMaxBusDelay)(uint64_t time);
//uint64_t            (*getMaxBusDelay)(void);
//void                (*setMaxIntDelay)(uint64_t time);
//uint64_t            (*getMaxIntDelay)(void);
//void                (*pmCPUSafeMode)(x86_lcpu_t *lcpu, uint32_t flags);
//void                (*pmTimerStateSave)(void);
//void                (*pmTimerStateRestore)(void);
//kern_return_t       (*exitHalt)(x86_lcpu_t *lcpu);
//kern_return_t       (*exitHaltToOff)(x86_lcpu_t *lcpu);
//void                (*markAllCPUsOff)(void);
//void                (*pmSetRunCount)(uint32_t count);
//boolean_t           (*pmIsCPUUnAvailable)(x86_lcpu_t *lcpu);
//int                 (*pmChooseCPU)(int startCPU, int endCPU, int preferredCPU);
//int                 (*pmIPIHandler)(void *state);
//void                (*pmThreadTellUrgency)(int urgency, uint64_t rt_period, uint64_t rt_deadline);
//void                (*pmActiveRTThreads)(boolean_t active);
//boolean_t           (*pmInterruptPrewakeApplicable)(void);
//void                (*pmThreadGoingOffCore)(thread_t old_thread, boolean_t transfer_load,
//                                              uint64_t last_dispatch, boolean_t thread_runnable);

pmDispatch_t pmRyzen_cpuFuncs = {
    .pmCPUStateInit = 0,
    .cstateInit = 0,
    .MachineIdle = &pmRyzen_machine_idle,
    .GetDeadline = 0,
    .SetDeadline = 0,
    .Deadline = 0,
#ifdef PMRYZEN_IDLE_MWAIT
    .exitIdle = &pmRyzen_exit_idle,
#else
    .exitIdle = 0,
#endif
    .markCPURunning = 0,
    .pmCPUControl = 0,
    .pmCPUHalt = 0,
    .getMaxSnoop = 0,
    .setMaxBusDelay = 0,
    .getMaxBusDelay = 0,
    .setMaxIntDelay = 0,
    .getMaxIntDelay = 0,
    .pmCPUSafeMode = 0,
    .pmTimerStateSave = 0,
    .pmTimerStateRestore = 0,
    .exitHalt = 0,
    .exitHaltToOff = 0,
    .markAllCPUsOff = 0,
    .pmSetRunCount = 0,
    .pmIsCPUUnAvailable = 0,
    .pmChooseCPU = &pmRyzen_choose_cpu,
    .pmIPIHandler = 0,
    .pmThreadTellUrgency = 0,
    .pmActiveRTThreads = 0,
    .pmInterruptPrewakeApplicable = 0,
    .pmThreadGoingOffCore = 0,
};

void pmRyzen_init_PState(){
    uint64_t p0 = pmRyzen_rdmsr_safe(pmRyzen_io_service_handle, MSR_PSTATE_0);
    
    // 0.7.6 (Round 3 M3): 原式 `(p0 & 0xff) / ((p0 >> 8) & 0x1f)` 是**整数除法**，
    // 分母为 0 时直接 #DE ⇒ 内核 panic（发生在 start 路径上）。
    // 分母为 0 的两个来源：pmRyzen_rdmsr_safe() 读失败返回 0；DfsId 字段本身为 0。
    // 读不到有效值就放弃改写固件（降级，不是崩溃）。
    // 注：DfsId 的正确掩码是 bits 13:8（6 位 0x3f），整式修正属 O13，随 T8 一并做。
    uint64_t p0dfs = (p0 >> 8) & 0x1f;
    if(p0dfs == 0) return;
    
    // 0.7.6 (Round 3 M3): 改成浮点除法 —— 原来的整数除法会截断小数部分，
    // 推出来的 P0 频率（进而 P1 FID）是错的。公式与 cpp 侧 dumpPstate() 一致：
    // clock = Fid / DfsId * 200。
    float p0spd = (float)(p0 & 0xff) / (float)p0dfs * 200.0F;
    
    uint64_t p1 = pmRyzen_rdmsr_safe(pmRyzen_io_service_handle, MSR_PSTATE_0 + 1);
    uint64_t p1dfs = (p1 >> 8) & 0x1f;
    // 0.7.6 (Round 3 M3): P1 的 DfsId 为 0 会推出 p1fid = 0，写回后 P1 频率为 0
    // （比不改写更糟）。同样放弃改写。
    if(p1dfs == 0) return;
    
    uint64_t p1fid = (uint64_t)((p0spd * 0.80F) / 200.0F * (float)p1dfs);
    // 0.7.6 (Round 3 M3): 值域保护 —— FID 只有 8 位。
    if(p1fid > 0xffULL) p1fid = 0xffULL;
    
    wrmsr64(MSR_PSTATE_0 + 1, (p1 & ~0xFFULL) | p1fid | (1ULL << 63));
}

inline void set_PState(pmProcessor_t *cpu, uint8_t state){
    if(pmRyzen_pstatelimit == 0) return;
    state = min(pmRyzen_pstatelimit, state);
    if(cpu->PState == state) return;
    
    boolean_t from_hpstate = !cpu->PState;
    
    pmRyzen_wrmsr_safe(pmRyzen_io_service_handle, MSR_PSTATE_CTL, state);
    cpu->PState = state;
    
    if(!state){
        __asm__ volatile("lock incq (%0)"::"r"(&pmRyzen_hpcpus):"memory");
    } else if(from_hpstate) {
        __asm__ volatile("lock decq (%0)"::"r"(&pmRyzen_hpcpus):"memory");
    }
}

void pmRyzen_doPState_reset(){
    uint32_t cn = cpu_number();
    // 0.7.6 (Round 3 M2): pmRyzen_cpus 是 XNU_MAX_CPU(64) 项的定长数组。
    // 逻辑核数超过数组长度时 cpu_number() 会越界 ⇒ 越界写破坏相邻内存。
    // 这类核在拓扑遍历时就没被注册（同样按 XNU_MAX_CPU 跳过），直接跳过。
    if(cn >= XNU_MAX_CPU) return;
    pmProcessor_t *self = &pmRyzen_cpus[cn];
    self->PState = 8;
    set_PState(self, 0);
}

void pmRyzen_PState_reset(){
    pmRyzen_hpcpus = 0;
    if(pmRyzen_pstatelimit == 0) pmRyzen_pstatelimit = 1;
    mp_rendezvous_no_intrs(&pmRyzen_doPState_reset, NULL);
}

boolean_t pmRyzen_init(void *handle){
    
    pmRyzen_io_service_handle = handle;
    
    // 0.7.6: 符号缺失时不能解引用。
    //
    // _pmDispatch / _pmUnRegister 缺失 ⇒ 无法注册 pmDispatch，本 kext 的
    // PM 能力整体不可用 ⇒ 初始化失败（调用方据此让 start() 返回 false，
    // 服务不发布），而不是空指针解引用把内核带崩。
    //
    // _tscFreq 缺失 ⇒ idle 回调里的时间基线（eff_timeacc / 阈值）没有意义。
    // 采用**优雅降级**（不是初始化失败）：见下面 _tscFreq 分支的说明。
    //
    // _i386_cpu_IPI 缺失只影响停止路径（发 IPI 唤醒 idle CPU），
    // 在调用点判空降级即可，不必让整个服务起不来。
    if(!pmRyzen_symtable._pmDispatch || !pmRyzen_symtable._pmUnRegister){
        IOLog("AMDCPUSupport::pmRyzen_init WARN: _pmDispatch/_pmUnRegister missing, PM registration impossible\n");
        pmRyzen_num_phys = 0;
        pmRyzen_num_logi = 0;
        return false;
    }
    
    // _tscFreq 缺失 ⇒ idle 回调里的时间基线（eff_timeacc / 阈值）没有意义。
    // 不让整个服务起不来，而是降级：pmRyzen_tsc_freq 保持 0，
    // pmRyzen_machine_idle() 会跳过基于负载的 P-state 自动伸缩
    // （否则阈值全为 0，CPU 会被钉死在 P0，既费电又发热）。
    // 包级功率测量同样由调用方（AMDRyzenCPUPowerManagement.cpp）单独降级。
    if(!pmRyzen_symtable._tscFreq){
        IOLog("AMDCPUSupport::pmRyzen_init WARN: _tscFreq missing, "
              "idle-based P-state scaling disabled\n");
        pmRyzen_tsc_freq = 0;
    }
    
    void **kernelDisp = pmRyzen_symtable._pmDispatch;
    pmRyzen_pmUnRegister = (void(*)(pmDispatch_t*))pmRyzen_symtable._pmUnRegister;
    pmRyzen_cpu_NMI = (void(*)(int))pmRyzen_symtable._cpu_NMI_interrupt;
    pmRyzen_NMI_enabled = (void(*)(boolean_t))pmRyzen_symtable._NMIPI_enable;
    pmRyzen_cpu_IPI = (void(*)(boolean_t))pmRyzen_symtable._i386_cpu_IPI;
    if(pmRyzen_symtable._tscFreq){
        pmRyzen_tsc_freq = *((uint64_t*)pmRyzen_symtable._tscFreq);
    }
    
    
    
    pmCallBacks_t cb;
    if(*kernelDisp)(*pmRyzen_pmUnRegister)(*kernelDisp);
    pmKextRegister(PM_DISPATCH_VERSION, &pmRyzen_cpuFuncs, &cb);
//    pmKextRegister(PM_DISPATCH_VERSION, NULL, &cb);
    
    x86_pkg_t * pkg = cb.GetPkgRoot();
    int pkgCount = 0;
    
    pmRyzen_num_phys = 0;
    pmRyzen_num_logi = 0;
    pmRyzen_hpcpus = 0;
    // enables low power state by default
    pmRyzen_pstatelimit = PSTATE_LOW_POWER;
    
    while(pkg){
        pkgCount++;
        x86_core_t *core = pkg->cores;
        
        while (core) {
            x86_lcpu_t *lcpu = core->lcpus;
            // 0.7.6 (Round 2 F2): 只统计"至少有一个 lcpu 注册成功"的物理核。
            // 原来无条件 pmRyzen_num_phys++，在 >64 逻辑核的机器上会出现
            // num_phys > num_logi（甚至 >64）—— 而用户态拿它当 effFreq_perCore
            // 的循环上界，会越界读并把相邻内存拷给用户态。
            int coreRegistered = 0;
            
            while (lcpu) {
                // 0.7.6: 越界检查。cpu_num 来自内核拓扑，理论上不会超过
                // XNU_MAX_CPU，但 pmRyzen_cpunum_to_lcpu / pmRyzen_cpus 都是
                // 定长数组，越界写会踩坏内存（而且这几个 helper 在全核屏障
                // 内被调用，踩坏的后果是整机死锁）。
                if(lcpu->cpu_num >= XNU_MAX_CPU){
                    IOLog("AMDCPUSupport::pmRyzen_init WARN: lcpu cpu_num %u out of range (max %u), skipped\n",
                          lcpu->cpu_num, (unsigned int)(XNU_MAX_CPU - 1));
                    lcpu = lcpu->next_in_core;
                    continue;
                }
                
                pmRyzen_num_logi++;
                coreRegistered = 1;
//                IOLog("LCPU: %u master:%u, primary:%u\n", lcpu->cpu_num, lcpu->master, lcpu->primary);
                
                pmRyzen_cpunum_to_lcpu[lcpu->cpu_num] = lcpu;
                pmProcessor_t *cpu = &pmRyzen_cpus[lcpu->cpu_num];
                cpu->lcpu = lcpu;
                cpu->stat_exit_idle = 0;
                cpu->arm_flag = 0;
                cpu->cpu_awake = 1;
                
                lcpu = lcpu->next_in_core;
            }
            
            if(coreRegistered) pmRyzen_num_phys++;
            
            core = core->next_in_pkg;
        }
        
        pkg = pkg->next;
    }
//    IOLog("pkg c %d\n", pkgCount);

    
    pmRyzen_effective_timetsc = ((double)pmRyzen_tsc_freq * EFF_INTERVAL);
    pmRyzen_p_sdtsc = (uint64_t)((double)pmRyzen_effective_timetsc * PSTATE_STEPDOWN_THRE);
    pmRyzen_p_sutsc = (uint64_t)((double)pmRyzen_effective_timetsc * PSTATE_STEPUP_THRE);
    
    pmRyzen_init_PState();
    pmRyzen_PState_reset();
    
    cb.initComplete();
    
    return true;
}

void pmRyzen_stop(){
    
    if(pmRyzen_pmUnRegister){
        (*pmRyzen_pmUnRegister)(&pmRyzen_cpuFuncs);
    }
    
    //Make sure all cores exited idle thread.
    //
    // 0.7.6: 这个循环以前可以永远转下去。
    // 非 MWAIT 构建下 pmRyzen_exit_idle() 恒返回 true，于是
    // while(exit_idle(...)) 永不退出 —— kext 停止路径上无限发 IPI，
    // 整机失去响应。现在 exit_idle() 会按 CPU 是否真的醒来返回，
    // 并且这里再加一道迭代次数上限兜底，任何情况下都能收敛。
    for (int i = 0; i < pmRyzen_num_logi; i++) {
        if(i >= XNU_MAX_CPU) break;
        x86_lcpu_t *lcpu = pmRyzen_cpunum_to_lcpu[i];
        if(lcpu == NULL) continue;
        
        int spins = 0;
        while(pmRyzen_exit_idle(lcpu)){
            if(pmRyzen_cpu_IPI){
                (*pmRyzen_cpu_IPI)(i);
            }
            if(++spins >= PMRYZEN_EXIT_IDLE_MAX_SPINS){
                IOLog("AMDCPUSupport::pmRyzen_stop: CPU %d did not leave idle after %d attempts, giving up\n",
                      i, spins);
                break;
            }
        }
    }
}



float pmRyzen_avgload_pcpu(uint32_t cpu){
    float loadacc = 0;
    int num_lcpus = 0;
    
    // 0.7.6 (Round 2 F3): 这里原来裸解引用 pmRyzen_cpunum_to_lcpu[cpu]->core，
    // 而本函数由用户态 selector 6 可达（索引由 logical/physical 推导）。
    // 若拓扑里 cpu_num 不连续（有空洞），命中空洞就是 NULL 解引用
    // —— 用户态一次调用即可把内核带崩。改走带边界/NULL 检查的 helper。
    x86_lcpu_t *entry = pmRyzen_get_lcpu(cpu);
    if(entry == NULL || entry->core == NULL) return 0.0f;
    
    x86_lcpu_t *lcpu = entry->core->lcpus;
    while (lcpu) {
        if(lcpu->cpu_num < XNU_MAX_CPU){
            float idleAccd = (float)pmRyzen_cpus[lcpu->cpu_num].eff_idleaccd;
            float timeAccd = (float)pmRyzen_cpus[lcpu->cpu_num].eff_timeaccd;
            // 0.7.6: timeAccd 可能为 0（还没到采样窗口），除法会产生 inf/NaN。
            if(timeAccd > 0)
                loadacc += 1 - idleAccd / timeAccd;
        }
        num_lcpus++;
        lcpu = lcpu->next_in_core;
    }
    
    return num_lcpus > 0 ? loadacc / (float)num_lcpus : 0.0f;
}


uint32_t pmRyzen_last_woken_cpu=0;
//uint32_t pmRyzen_last_idle_cpu=0;

// 0.7.6 (Round 3 M2): pmRyzen_cpus[] 是 XNU_MAX_CPU(64) 项的定长数组。
// 逻辑核数超过 64 时 cpu_number() 会越界。这类核在拓扑遍历时没被注册
// （没有逐核记账的意义），但 idle 回调照样会被 XNU 调用 —— 不能直接返回，
// 那会让这些核永远空转。退化方案：状态指向这个静态暂存结构，记账写进去
// （无意义但无害），idle 等待照常执行。
static pmProcessor_t pmRyzen_untracked_cpu;

uint64_t pmRyzen_machine_idle(uint64_t maxDur){

    uint32_t cn = cpu_number();
    bool tracked = (cn < XNU_MAX_CPU);
    pmProcessor_t *self = tracked ? &pmRyzen_cpus[cn] : &pmRyzen_untracked_cpu;
    
//    pmRyzen_last_idle_cpu = cn;
    
    __asm__ volatile("cli;");
    
    self->cpu_awake = 0;
    self->arm_flag = 0;
    
    uint64_t tscnow = rdtsc64();

    self->last_idle_tsc = tscnow;
//    self->last_running_time = self->last_idle_tsc - self->last_start_tsc;
    
#ifdef PMRYZEN_IDLE_MWAIT

    void* addr = &self->arm_flag;
    uint32_t ps_hint = 0x50;
    __asm__ volatile("wbinvd":::"memory");
    __asm__ volatile("mfence":::"memory");
    __asm__ volatile("clflushopt %0" : "+m" (*(volatile char *)&self->arm_flag));

    __asm__ volatile("mfence;"
                     "movq %0, %%rax;"
                     "xor %%edx, %%edx;"
                     "xor %%ecx, %%ecx;"
                     "monitor;"
                     "xorq %%rax, %%rax;"
                     "movl %1, %%eax;"
                     "movl $0x1, %%ecx;"
                     "mwait;"
                     :
                     : "r"(addr), "r"(ps_hint)
                     : "%ecx", "%edx", "%rax"
                     );
    
#elif PMRYZEN_IDLE_SIMPLE
    
    __asm__ volatile("sti;hlt;");
    
#elif PMRYZEN_IDLE_IO_CSTATE
    
    __asm__ volatile("sti;"
                     "inw $0xf2, %%ax;"
                     "cli;"
                     :::"%eax");
    
#endif

    
    self->cpu_awake = 1;
    if(!self->arm_flag)
        pmRyzen_exit_idle_false_c++;
    
    
    tscnow = rdtsc64();

    uint64_t tscela = tscnow - self->last_idle_tsc;
    self->eff_timeacc += tscnow - self->last_start_tsc;
    self->eff_idleacc += tscela;

    // 0.7.6: pmRyzen_tsc_freq == 0 表示 _tscFreq 符号缺失、时间基线不可用。
    // 此时 pmRyzen_effective_timetsc / p_sutsc / p_sdtsc 全为 0，
    // 走进这个分支会永远走"升档"分支（rt > 0），把 CPU 钉死在 P0。
    // 所以这里直接跳过基于负载的 P-state 自动伸缩（降级，不是崩溃）。
    // 0.7.6 (Round 3 M2): 未注册的核（tracked == false）不参与自动伸缩 ——
    // 它们共用同一个暂存结构，负载历史没有意义。
    if(tracked && pmRyzen_tsc_freq > 0 && self->eff_timeacc > pmRyzen_effective_timetsc){
//        self->eff_load = 1 - (float)self->eff_idleacc / (float)self->eff_timeacc;
        uint64_t rt = self->eff_timeacc - self->eff_idleacc;
        
        //Avoid using xmm registers shared within same core.
        if(rt > pmRyzen_p_sutsc){
            if (self->PState > 0) {
                set_PState(self, self->PState-1);
            } else {
                set_PState(self, 0);
            }
            self->ll_count = 0;
        } else if(rt < pmRyzen_p_sdtsc){
            self->ll_count++;
            if(self->ll_count > PSTATE_STEPDOWN_TIME + pmRyzen_hpcpus * PSTATE_STEPDOWN_MP_GAIN){
                self->ll_count = 0;
                set_PState(self, self->PState+1);
            }
        }
        
//        self->eff_load = 1 - (float)self->eff_idleacc / (float)self->eff_timeacc;
        self->eff_idleaccd = self->eff_idleacc;
        self->eff_timeaccd = self->eff_timeacc;
        self->eff_timeacc = 0;
        self->eff_idleacc = 0;

        
//        if(self->eff_load > PSTATE_STEPUP_THRE){
//            set_PState(self, 0);
//            self->ll_count = 0;
//        } else if(self->eff_load < PSTATE_STEPDOWN_THRE){
//            self->ll_count++;
//            if(self->ll_count > PSTATE_STEPDOWN_TIME){
//                set_PState(self, self->PState+1);
//            }
//        }

    }

    self->last_start_tsc = tscnow;
    self->last_idle_length = tscela;
    
    // 0.7.6 (Round 3 M2): 只记录已注册的核，保证 pmRyzen_last_woken_cpu
    // 永远 < XNU_MAX_CPU（pmRyzen_choose_cpu() 会用它索引 pmRyzen_cpus[]）。
    if(tracked) pmRyzen_last_woken_cpu = cn;
    return 0;
}

boolean_t pmRyzen_exit_idle(x86_lcpu_t *lcpu){
    
    // 0.7.6: 越界检查（pmRyzen_cpus 是定长数组）。
    if(lcpu == NULL || lcpu->cpu_num >= XNU_MAX_CPU) return false;
    
    pmProcessor_t *target = &pmRyzen_cpus[lcpu->cpu_num];

    // Exit if cpu is already awake.
//    if(target->cpu_awake) return false;
    
//    pmRyzen_exit_idle_c++;
    
#ifdef PMRYZEN_IDLE_MWAIT
    uint64_t start_tsc = rdtsc64();
    do {
        target->arm_flag = 1;
        __asm__ volatile("pause;");
//        asm volatile("clflushopt %0" : "+m" (*(volatile char *)&target->arm_flag));
//        __asm__ volatile("mfence;");

        if(rdtsc64() - start_tsc > 0x6000){
            //If we still unable to wake up the processor, send an IPI.
            pmRyzen_exit_idle_ipi_c++;
            return true;
        }
    } while(!target->cpu_awake);
    
    return false;
    
#else
    target->arm_flag = 1;
//    pmRyzen_exit_idle_ipi_c++;
    
    // 0.7.6: 非 MWAIT 构建下没有 monitor/mwait，CPU 只能靠挂起的中断离开
    // idle 状态。以前这里无条件 return true —— pmRyzen_stop() 的
    // while(exit_idle(...)) 因此永远转下去（每次迭代还发一个 IPI），
    // kext 停止路径上整机挂死。
    // 现在按 CPU 是否真的醒来返回：还在 idle 里就继续报 true。
    if(target->cpu_awake){
        pmRyzen_exit_idle_false_c++;
        return false;
    }
    
    return true;
#endif
}

int pmRyzen_choose_cpu(int startCPU, int endCPU, int preferredCPU){
    
    //We only provide a hint as scheduler will make the final decision anyway.
  
//    return preferredCPU;
    
    // 0.7.6 (Round 3 M2): preferredCPU 由 XNU 传入，可能超出 pmRyzen_cpus[]
    // 的范围（逻辑核数 > 64）⇒ 越界读。pmRyzen_last_woken_cpu 同理。
    if(preferredCPU >= 0 && preferredCPU < XNU_MAX_CPU && pmRyzen_cpus[preferredCPU].cpu_awake)
        return preferredCPU;

    if(pmRyzen_last_woken_cpu < XNU_MAX_CPU && pmRyzen_cpus[pmRyzen_last_woken_cpu].cpu_awake)
        return pmRyzen_last_woken_cpu;
    
//    for(int i = startCPU; i < endCPU; i++){
//        if(i != pmRyzen_last_idle_cpu && pmRyzen_cpus[i].cpu_awake)
//            return i;
//    }
    
    return preferredCPU;
}

pmProcessor_t* pmRyzen_get_processor(uint32_t cpu){
    // 0.7.6 (Round 3 M2): 越界检查（pmRyzen_cpus 是定长数组）。
    if(cpu >= XNU_MAX_CPU) return NULL;
    return &pmRyzen_cpus[cpu];
}
