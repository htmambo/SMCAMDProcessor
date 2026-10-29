#include "AMDRyzenCPUPowerManagement.hpp"
#include <string.h>

OSDefineMetaClassAndStructors(AMDRyzenCPUPowerManagement, IOService);

#define TCTL_OFFSET_TABLE_LEN 6
static constexpr const struct tctl_offset tctl_offset_table[] = {
    { 0x17, "AMD Ryzen 5 1600X", 20 },
    { 0x17, "AMD Ryzen 7 1700X", 20 },
    { 0x17, "AMD Ryzen 7 1800X", 20 },
    { 0x17, "AMD Ryzen 7 2700X", 10 },
    { 0x17, "AMD Ryzen Threadripper 19", 27 }, /* 19{00,20,50}X */
    { 0x17, "AMD Ryzen Threadripper 29", 27 }, /* 29{20,50,70,90}[W]X */
};

bool ADDPR(debugEnabled) = false;
uint32_t ADDPR(debugPrintDelay) = 0;

// 0.7.6: 符号查找的最大**重试**次数（不含首次查找 ⇒ 总共 21 次查找）。
// 以前是无限重试：符号一旦不存在（未来的 macOS、符号表变化、符号被裁剪），
// kext 会永远卡在 init() 里，服务永远不发布 —— 表现为"某个 kext 不见了"。
// 0.7.6 (Round 2 F5)：从"无限重试（每次 2 ms）"收紧到有上限。
// 0.7.6 (Round 3 m1)：原注释写"每次间隔 1 s ⇒ 上限约 20 s"，与实现不符 ——
// 本常量的单位是**毫秒**，实际总等待只有 ≈ 20 ms。这里按 20 ms 而不是 20 s：
// _wrmsr_carefully 是内核静态符号表里的符号，要么存在要么不存在，
// 不存在时不会"晚点出现"，多等毫无意义，只会把开机拖长
// （macOS 新版本删掉该私有符号时，每次开机都要白等 20 s）。
static constexpr uint32_t kSymbolLookupMaxAttempts = 20;
static constexpr uint32_t kSymbolLookupRetryDelayMs = 1;

// 0.7.6: HWCR 初始化的每 CPU 重试预算（每次唤醒重置）。
// 唤醒路径上的重试必须有界：一个永久读失败的 HWCR 否则会在每个 tick 的
// 无中断屏障里反复读+写，暴露窗口没有终点。
static constexpr uint8_t PMRYZEN_HWCR_RETRY_BUDGET = 20;


extern "C"{
void pmRyzen_wrmsr_safe(void *handle, uint32_t addr, uint64_t value){
    static_cast<AMDRyzenCPUPowerManagement*>(handle)->write_msr(addr, value);
}

uint64_t pmRyzen_rdmsr_safe(void *handle, uint32_t addr){
    uint64_t v = 0;
    static_cast<AMDRyzenCPUPowerManagement*>(handle)->read_msr(addr, &v);
    return v;
}

pmRyzen_symtable_t pmRyzen_symtable={0};
// 0.7.6: pmRyzen_symtable_ready 已删除 —— 它只写不读（等待它的循环早已被
// 注释掉），留着只会误导下一个人以为还有同步点。

}


bool AMDRyzenCPUPowerManagement::init(OSDictionary *dictionary){
//    strcpy((char*)kMODULE_VERSION, xStringify(MODULE_VERSION), (uint32_t)strlen(xStringify(MODULE_VERSION)));
    IOLog("AMDCPUSupport v%s, init\n", xStringify(MODULE_VERSION));
    
    IOLog("AMDCPUSupport::enter dlinking..\n");
    
    // 0.7.6: 查找必须有上限。超限后不再等待 —— write_msr() 自带裸 wrmsr64
    // 兜底路径（见 write_msr），服务可以照常发布，只是失去"安全写"能力。
    uint32_t lookupAttempts = 0;
    
retry:
    find_mach_header_addr(getKernelVersion() >= KernelVersion::BigSur);
    pmRyzen_symtable._wrmsr_carefully = lookup_symbol("_wrmsr_carefully");
    
    if(!pmRyzen_symtable._wrmsr_carefully){
        // 0.7.6 (Round 2 F5): kextloadAlerts 只在首次失败时自增一次。
        // 原来每轮重试都 ++，用户看到的弹窗会变成 "Kext alert detected: 21"
        // —— 语义失真（那不是 21 个告警，是同一个告警被重试了很多次）。
        if(lookupAttempts == 0) kextloadAlerts++;
        if(lookupAttempts++ < kSymbolLookupMaxAttempts){
            IOSleep(kSymbolLookupRetryDelayMs);
            goto retry;
        }
        // 0.7.6 (Round 3 m1): 日志同时给出实际查找次数与重试上限 ——
        // 循环是"先判断再自增"，退出时 lookupAttempts = 上限 + 1，
        // 只打印其中一个数字会让人对不上代码。
        IOLog("AMDCPUSupport::init WARN: _wrmsr_carefully not found after %u attempt(s) "
              "(max retries %u), falling back to unsafe wrmsr64\n",
              lookupAttempts, kSymbolLookupMaxAttempts);
    }
    
    
    pmRyzen_symtable._KUNCUserNotificationDisplayAlert = lookup_symbol("_KUNCUserNotificationDisplayAlert");
    pmRyzen_symtable._cpu_to_processor = lookup_symbol("_cpu_to_processor");
    pmRyzen_symtable._tscFreq = lookup_symbol("_tscFreq");
    pmRyzen_symtable._pmDispatch = lookup_symbol("_pmDispatch");
    pmRyzen_symtable._pmUnRegister = lookup_symbol("_pmUnRegister");
    pmRyzen_symtable._cpu_NMI_interrupt = lookup_symbol("_cpu_NMI_interrupt");
    pmRyzen_symtable._NMIPI_enable = lookup_symbol("_NMIPI_enable");
    pmRyzen_symtable._i386_cpu_IPI = lookup_symbol("_i386_cpu_IPI");
    IOLog("AMDCPUSupport::enter link finished.\n");
    return IOService::init(dictionary);
}

void AMDRyzenCPUPowerManagement::free(){
    IOService::free();
}


bool AMDRyzenCPUPowerManagement::getPCIService(){
    
    
    OSDictionary *matching_dict = serviceMatching("IOPCIDevice");
    if(!matching_dict){
        IOLog("AMDCPUSupport::getPCIService: serviceMatching unable to generate matching dictonary.\n");
        return false;
    }
    
    //Wait for PCI services to init.
    waitForMatchingService(matching_dict);
    
    OSIterator *service_iter = getMatchingServices(matching_dict);
    IOPCIDevice *service = 0;
    
    if(!service_iter){
        IOLog("AMDCPUSupport::getPCIService: unable to find a matching IOPCIDevice.\n");
        return false;
    }
    
    while (true){
        OSObject *obj = service_iter->getNextObject();
        if(!obj) break;
        
        service = OSDynamicCast(IOPCIDevice, obj);
        break;
    }
    
    if(!service){
        IOLog("AMDCPUSupport::getPCIService: unable to get IOPCIDevice on host system.\n");
        return false;
    }
    
    IOLog("AMDCPUSupport::getPCIService: succeed!\n");
    fIOPCIDevice = service;
    
    return true;
}

void AMDRyzenCPUPowerManagement::initWorkLoop() {
    IOLog("AMDCPUSupport::startWorkLoop setting up timer");
    
    timerEvent_main = IOTimerEventSource::timerEventSource(this, [](OSObject *object, IOTimerEventSource *sender) {
        AMDRyzenCPUPowerManagement *provider = OSDynamicCast(AMDRyzenCPUPowerManagement, object);

        //Run initialization
        if(!provider->serviceInitialized){
            IOLog("AMDCPUSupport::startWorkLoop initialize service");
            
            //Disable interrupts and sync all processor cores.
            mp_rendezvous_no_intrs([](void *obj) {
                auto provider = static_cast<AMDRyzenCPUPowerManagement*>(obj);
                
                provider->write_msr(kMSR_CSTATE_ADDR, 0xf0);
                
//                uint64_t val = 0;
//                provider->read_msr(0xC0010292, &val);
//                provider->write_msr(0xC0010292, val | ((uint64_t)1 << 32));
//
//                provider->read_msr(0xC0010296, &val);
//                provider->write_msr(0xC0010296, val | (1 << 22) | (1 << 14) | (1 << 6));

                uint32_t cpu_num = cpu_number();

                // 0.7.6 (Round 2 F1): cpu_number() 在超过 64 个逻辑核的机器上
                // 可以 >= CPUInfo::MaxCpus，而下面所有 per-CPU 数组都是
                // 定长 64 项。直接拿它当下标就是越界写 —— 而且发生在全核
                // 关中断屏障内（内存损坏 + 整机死锁）。越界的核直接跳过。
                if(cpu_num >= CPUInfo::MaxCpus) return;

                uint64_t hwConfig;
                // Never panic here: this block re-runs on every wake from sleep.
                // A failed read — or a failed write — is remembered per CPU and
                // retried by the regular timer tick below, so the init is not
                // lost for this wake cycle.
                //
                // 0.7.6: 每次唤醒重置该 CPU 的重试预算 —— 唤醒路径上的重试
                // 次数必须有界，否则一次永久性读失败会在每个 tick（50–1200 ms）
                // 的无中断屏障里反复做一次读 + 一次写，直到天荒地老。
                provider->hwcrInitRetryLeft[cpu_num] = PMRYZEN_HWCR_RETRY_BUDGET;
                
                if(provider->read_msr(kMSR_HWCR, &hwConfig)){
                    hwConfig |= (1 << 30);
                    provider->hwcrInitPending[cpu_num] = !provider->write_msr(kMSR_HWCR, hwConfig);
                } else {
                    provider->hwcrInitPending[cpu_num] = true;
                }


                //Read PStateDef generated by EFI.
                if(pmRyzen_cpu_is_master(cpu_num)){
                    provider->pstateDumpRetryLeft = 20;
                    provider->pstateDumpPending = !provider->dumpPstate(nullptr, false);
                }


                if(!pmRyzen_cpu_primary_in_core(cpu_num)) return;
                uint32_t physicalRaw = pmRyzen_cpu_phys_num(cpu_num);
                // 0.7.6 (Round 2 F1): 物理核下标同样可能超出定长数组
                // （>64 物理核的机器上 pcore_num 可达 255）。
                if(physicalRaw >= CPUInfo::MaxCpus) return;
                uint8_t physical = (uint8_t)physicalRaw;


                //Init performance frequency counter.
                uint64_t APERF, MPERF;
                if(provider->read_msr(kMSR_APERF, &APERF) && provider->read_msr(kMSR_MPERF, &MPERF)){
                    provider->lastAPERF_perCore[physical] = APERF;
                    provider->lastMPERF_perCore[physical] = MPERF;
                    provider->freqCountersSeeded[physical] = true;
                }

            }, provider);
            
            // 0.7.5: post-wake HWCR snapshot for drift monitoring.
            // Deliberately done OUTSIDE the mp_rendezvous_no_intrs barrier:
            // IOLog takes the console/os_log global lock, and XNU asserts
            // "IOLog called with interrupts disabled" — inside that barrier
            // every CPU runs with interrupts disabled.
            if (ADDPR(debugEnabled)) {
                uint64_t hwcr = 0;
                if (provider->read_msr(kMSR_HWCR, &hwcr)) {
                    IOLog("AMDCPUSupport::init post-wake HWCR=0x%llx CpbDis=%u\n",
                          hwcr, (unsigned int)((hwcr >> 25) & 1U));
                }
                uint32_t pending = 0;
                for (uint32_t i = 0; i < CPUInfo::MaxCpus; i++) {
                    if (provider->hwcrInitPending[i]) pending++;
                }
                if (pending > 0) {
                    IOLog("AMDCPUSupport::init HWCR init pending on %u CPU(s), will retry\n", pending);
                }
            }
            
            //Make all cores P0 state by default.
            provider->PStateCtl = 0;
            
            provider->serviceInitialized = true;
            provider->timerEvent_main->setTimeoutMS(1);
            return;
        }
        
        
        mp_rendezvous_no_intrs([](void *obj) {
            auto provider = static_cast<AMDRyzenCPUPowerManagement*>(obj);
            uint32_t cpu_num = cpu_number();

            // 0.7.6 (Round 2 F1): per-CPU 数组都是定长 64 项，
            // cpu_number() 在 >64 逻辑核的机器上可以越界 —— 越界的核跳过。
            if(cpu_num >= CPUInfo::MaxCpus) return;

            // 0.7.5: retry the HWCR init on CPUs where the read or write
            // failed before. Every CPU only touches its own slot, so there is
            // no cross-CPU race. The master CPU also retries a failed P-state
            // table dump so a transient read failure is not silently final.
            //
            // 0.7.6: 重试次数现在有界（每 CPU 一个预算，唤醒时重置）。
            // 以前是无界的：一个永久读失败的 HWCR 会让每个 tick 都在
            // 无中断屏障里做一次读 + 一次写，永远下去。
            if(provider->hwcrInitPending[cpu_num]){
                if(provider->hwcrInitRetryLeft[cpu_num] > 0){
                    provider->hwcrInitRetryLeft[cpu_num]--;
                    uint64_t hwConfig;
                    if(provider->read_msr(kMSR_HWCR, &hwConfig)){
                        hwConfig |= (1 << 30);
                        if(provider->write_msr(kMSR_HWCR, hwConfig))
                            provider->hwcrInitPending[cpu_num] = false;
                    }
                }
            }

            if(provider->pstateDumpPending && pmRyzen_cpu_is_master(cpu_num)){
                // Bounded: every rdmsr takes cpu_lock inside the barrier, so
                // a permanently unreadable P-state MSR must not retry every
                // tick forever. The budget is refilled on wake re-init and
                // on explicit user writePstate().
                if(provider->pstateDumpRetryLeft > 0){
                    provider->pstateDumpRetryLeft--;
                    provider->pstateDumpPending = !provider->dumpPstate(nullptr, false);
                    // 0.7.6: 屏障内不做 IOLog —— IOLog 会取全局日志锁，
                    // 而这里所有核都关着中断（XNU 还会 assert
                    // "IOLog called with interrupts disabled"）。
                    // 这里只置标志，由屏障外的调用方打印。
                    if(provider->pstateDumpPending && provider->pstateDumpRetryLeft == 0)
                        provider->pstateDumpRetryExhausted = true;
                }
            }

            provider->updateInstructionDelta(cpu_num);

            // Ignore hyper-threaded cores
            if(!pmRyzen_cpu_primary_in_core(cpu_num)) return;
            uint32_t physicalRaw = pmRyzen_cpu_phys_num(cpu_num);
            // 0.7.6 (Round 2 F1): 物理核下标钳位（同上）。
            if(physicalRaw >= CPUInfo::MaxCpus) return;
            uint8_t physical = (uint8_t)physicalRaw;


            provider->calculateEffectiveFrequency(physical);

        }, provider);
        
        // 0.7.6: 屏障外的报告点。屏障内所有核都关着中断，IOLog 会取全局
        // 日志锁（XNU 还会 assert "IOLog called with interrupts disabled"），
        // 因此重试耗尽的消息必须在这里打印，而不是在屏障内。
        if(provider->pstateDumpRetryExhausted){
            provider->pstateDumpRetryExhausted = false;
            if (ADDPR(debugEnabled))
                IOLog("AMDCPUSupport::initWorkLoop: P-state dump retries exhausted, keeping last-good table\n");
        }
        
        //Read stats from package.
        provider->updatePackageTemp();
        provider->updatePackageEnergy();
        

        
        uint32_t now = uint32_t(getCurrentTimeNs() / 1000000); //ms
        uint32_t newInt = max(now - provider->timeOfLastMissedRequest,
                              provider->estimatedRequestTimeInterval);

        provider->actualUpdateTimeInterval = now - provider->timeOfLastUpdate;
        provider->timeOfLastUpdate = now;
        provider->updateTimeInterval = min(1200, max(50, newInt));
        
        provider->timerEvent_main->setTimeoutMS(provider->updateTimeInterval);

//        IOLog("fpp %d %d %.4f.\n", HF_TEMP_SAMPLE_FREQ, HF_TEMP_SAMPLE_PERIOD, (float)HF_TEMP_SAMPLE_REP);
        
    });
    
//    tempSamplePeriod = (int)((1.0f / (float)HF_TEMP_SAMPLE_FREQ) * 1000);
    float fillT = getPackageTemp();
    tempNextSample = 0;
    for (int i = 0; i < HF_TEMP_SAMPLE_LEN; i++) tempSamples[i] = fillT;
    
    timerEvent_tempe = IOTimerEventSource::timerEventSource(this, [](OSObject *object, IOTimerEventSource *sender) {
        AMDRyzenCPUPowerManagement *provider = OSDynamicCast(AMDRyzenCPUPowerManagement, object);
        
        int next_samp = provider->tempNextSample;
        float t = provider->getPackageTemp();
        provider->tempSamples[next_samp] = t;
        provider->tempNextSample = (next_samp + 1) % HF_TEMP_SAMPLE_LEN;
        
        sender->setTimeoutMS(HF_TEMP_SAMPLE_PERIOD);
    });
    
    registerService();
    
    lastUpdateTime = getCurrentTimeNs();
    pwrLastTSC = rdtsc64();
    workLoop->addEventSource(timerEvent_main);
    workLoop->addEventSource(timerEvent_tempe);
    timerEvent_main->setTimeoutMS(1);
    timerEvent_tempe->setTimeoutMS(HF_TEMP_SAMPLE_PERIOD);
}

void AMDRyzenCPUPowerManagement::stopWorkLoop() {
//    IOLog("AMDCPUSupport::startWorkLoop stopping timer");

    
    workLoop->disableAllEventSources();
    serviceInitialized = false;
}

void AMDRyzenCPUPowerManagement::resumeWorkLoop() {
    workLoop->enableAllEventSources();
}

bool AMDRyzenCPUPowerManagement::start(IOService *provider){
    
    bool success = IOService::start(provider);
    if(!success){
        IOLog("AMDCPUSupport::start failed to start. :(\n");
        return false;
    }
    
    disablePrivilegeCheck = checkKernelArgument("-amdpnopchk");
    
    uint32_t cpuid_eax = 0;
    uint32_t cpuid_ebx = 0;
    uint32_t cpuid_ecx = 0;
    uint32_t cpuid_edx = 0;
    CPUInfo::getCpuid(0, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    IOLog("AMDCPUSupport::start got CPUID: %X %X %X %X\n", cpuid_eax, cpuid_ebx, cpuid_ecx, cpuid_edx);
    
    if(cpuid_ebx != CPUInfo::signature_AMD_ebx
       || cpuid_ecx != CPUInfo::signature_AMD_ecx
       || cpuid_edx != CPUInfo::signature_AMD_edx){
        IOLog("AMDCPUSupport::start no AMD signature detected, failing..\n");
        
        return false;
    }
    
    CPUInfo::getCpuid(1, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    cpuFamily = ((cpuid_eax >> 20) & 0xff) + ((cpuid_eax >> 8) & 0xf);
    cpuModel = ((cpuid_eax >> 16) & 0xf) + ((cpuid_eax >> 4) & 0xf);
    
    //Only 17h Family are supported offically by now.
    // EDIT: add check bypass for 19h Zen 3 processors
    cpuSupportedByCurrentVersion = (cpuFamily == 0x17 || cpuFamily == 0x19)? 1 : 0;
    IOLog("AMDCPUSupport::start Family %02Xh, Model %02Xh\n", cpuFamily, cpuModel);
    
    CPUInfo::getCpuid(0x80000005, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    // 0.7.6: 复制粘贴错误修正。CPUID Fn80000005: ECX[31:24] = L1 数据缓存
    // 每核容量(KB)，EDX[31:24] = L1 指令缓存每核容量(KB)。原来把 ECX 加了
    // 两次（数据缓存翻倍、指令缓存丢失），启动日志里的 L1 容量是错的。
    cpuCacheL1_perCore = (cpuid_ecx >> 24) + (cpuid_edx >> 24);
    
    
    CPUInfo::getCpuid(0x80000006, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    cpuCacheL2_perCore = (cpuid_ecx >> 16);
    cpuCacheL3 = (cpuid_edx >> 18) * 512;
    IOLog("AMDCPUSupport::start L1: %u, L2: %u, L3: %u\n",
          cpuCacheL1_perCore, cpuCacheL2_perCore, cpuCacheL3);
    
    
    CPUInfo::getCpuid(0x80000007, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    cpbSupported = (cpuid_edx >> 9) & 0x1;

    
    CPUInfo::getCpuid(0x00000005, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    IOLog("AMDCPUSupport::start CPUID MWait: %X %X %X %X\n", cpuid_eax, cpuid_ebx, cpuid_ecx, cpuid_edx);
    
    uint32_t nameString[12]{};
    CPUInfo::getCpuid(0x80000002, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    nameString[0] = cpuid_eax; nameString[1] = cpuid_ebx; nameString[2] = cpuid_ecx; nameString[3] = cpuid_edx;
    CPUInfo::getCpuid(0x80000003, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    nameString[4] = cpuid_eax; nameString[5] = cpuid_ebx; nameString[6] = cpuid_ecx; nameString[7] = cpuid_edx;
    CPUInfo::getCpuid(0x80000004, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    nameString[8] = cpuid_eax; nameString[9] = cpuid_ebx; nameString[10] = cpuid_ecx; nameString[11] = cpuid_edx;
    
    IOLog("AMDCPUSupport::start Processor: %s))\n", (char*)nameString);
    
    //Check tctl temperature offset
    for(int i = 0; i < TCTL_OFFSET_TABLE_LEN; i++){
        const TempOffset *to = tctl_offset_table + i;
        if(cpuFamily == to->model && strstr((char*)nameString, to->id)){
            
            tempOffset = (float)to->offset;
            break;
        }
    }
    
    uint64_t rapl = 0;
    // 0.7.6 (Round 2 P2): 原来是 panic() —— start() 路径上的内核 panic。
    // 与"缺失即降级"的原则冲突：读不到 RAPL 单位只影响包级功率读数，
    // 不该把整台机器带走。降级为"功率测量不可用"（updatePackageEnergy 跳过）。
    if(!read_msr(kMSR_RAPL_PWR_UNIT, &rapl)){
        IOLog("AMDCPUSupport::start WARN: unable to read power unit, "
              "package power measurement disabled\n");
        raplValid = false;
    } else {
        raplValid = true;
    }
    
    // AMD MSRC001_0299 (RAPL Power Unit) 的位域：
    //   TU [19:16] / ESU [12:8] / PU [3:0]
    // 下面的掩码与该布局一致（ESU 5 位、TU 4 位）。
    pwrTimeUnit = pow((double)0.5, (double)((rapl >> 16) & 0xf));
    pwrEnergyUnit = pow((double)0.5, (double)((rapl >> 8) & 0x1f));
//    IOLog("a %lld\n", (long long)(pwrTimeUnit * 10000000000));
//    IOLog("b %lld\n", (long long)(pwrEnergyUnit * 10000000000));
    
    fetchOEMBaseBoardInfo();
    
//    if(!CPUInfo::getCpuTopology(cpuTopology)){
//        IOLog("AMDCPUSupport::start unable to get CPU Topology.\n");
//    }
//    IOLog("AMDCPUSupport::start got %hhu CPU(s): Physical Count: %hhu, Logical Count %hhu.\n",
//          cpuTopology.packageCount, cpuTopology.totalPhysical(), cpuTopology.totalLogical());
//
//    totalNumberOfPhysicalCores = cpuTopology.totalPhysical();
//    totalNumberOfLogicalCores = cpuTopology.totalLogical();
    
    
    IOLog("AMDCPUSupport::start trying to init PCI service...\n");
    if(!getPCIService()){
        IOLog("AMDCPUSupport::start no PCI support found, failing...\n");
        return false;
    }
    
// 0.7.6: 原来这里有一个等待 pmRyzen_symtable_ready 的死循环。
// 该变量只写不读（等待它的代码早已被注释掉），已随清理一并删除。
    
    void *safe_wrmsr = pmRyzen_symtable._wrmsr_carefully;
    if(!safe_wrmsr){
        IOLog("AMDCPUSupport::start WARN: Can't find _wrmsr_carefully, proceeding with unsafe wrmsr\n");
    } else {
        wrmsr_carefully = (int(*)(uint32_t,uint32_t,uint32_t)) safe_wrmsr;
    }

    void *_kunc_alert = pmRyzen_symtable._KUNCUserNotificationDisplayAlert;
    IOLog("kunc_alert %p\n", kunc_alert);
    if(!_kunc_alert){
        IOLog("AMDCPUSupport::start WARN: Can't find _KUNCUserNotificationDisplayAlert.\n");
    } else {
        kunc_alert =
        (kern_return_t(*)(int,unsigned,const char*,const char*,const char*,
        const char*,const char*,const char*,const char*,const char*,unsigned*))_kunc_alert;
    }

    cpu_to_processor = (processor_t(*)(int))pmRyzen_symtable._cpu_to_processor;
    if(!pmRyzen_symtable._cpu_to_processor){
        IOLog("AMDCPUSupport::start WARN: Can't find _cpu_to_processor\n");
    }

    // 0.7.6: _tscFreq 是一个指向内核变量的指针。符号缺失时直接解引用
    // 会在启动阶段把内核带崩（start() 里的空指针解引用）。
    // 现在缺失时保留默认值并关闭依赖它的包级功率测量。
    if(pmRyzen_symtable._tscFreq){
        xnuTSCFreq = *((uint64_t*)pmRyzen_symtable._tscFreq);
        tscFreqValid = true;
    } else {
        IOLog("AMDCPUSupport::start WARN: Can't find _tscFreq, package power measurement disabled\n");
    }

    // 0.7.6: pmRyzen_init() 失败（PM 相关符号缺失）时不能继续发布服务：
    // 带着 0 个物理核发布出去，用户态的 logical/physical 除法会直接
    // 把内核带崩（整数除零）。这里让 start() 失败，App 会提示 kext 未加载。
    if(!pmRyzen_init(this)){
        IOLog("AMDCPUSupport::start failed: PM registration unavailable\n");
        return false;
    }
    
    totalNumberOfLogicalCores = pmRyzen_num_logi;
    totalNumberOfPhysicalCores = pmRyzen_num_phys;
    
    if(totalNumberOfPhysicalCores == 0 || totalNumberOfLogicalCores == 0
       || totalNumberOfPhysicalCores > CPUInfo::MaxCpus){
        IOLog("AMDCPUSupport::start failed: bad CPU topology (%u physical / %u logical)\n",
              totalNumberOfPhysicalCores, totalNumberOfLogicalCores);
        // 0.7.6 (Round 2 F7): pmRyzen_init() 此时已经注册过 pmDispatch。
        // 直接返回 false 会让 XNU 侧残留 pmRyzen_machine_idle 回调
        // （指向一个 pmRyzen_cpus[] 全零的半初始化对象）⇒ 显式注销。
        pmRyzen_stop();
        return false;
    }

    IOLog("AMDCPUSupport::start, Physical Count: %u, Logical Count %u.\n",
              totalNumberOfPhysicalCores, totalNumberOfLogicalCores);

    workLoop = IOWorkLoop::workLoop();
    initWorkLoop();

	// 初始化时是否关闭CPB
    uint8_t customValue = 0;
    OSNumber *arg;
    arg = OSDynamicCast(OSNumber, getProperty("CPBStatus"));
    if(arg==NULL){
        customValue = 0;
    } else {
        customValue = arg->unsigned8BitValue();
    }
    // sleepState.cpb caches the DESIRED CPB state (what the wake path should
    // (re)apply), not necessarily the current hardware state: if the
    // setCPBState() write below fails, restoreWakeState() retries applying
    // this value on the next wake.
    sleepState.cpb = customValue>=1;
    // 实际用到的其实并不是频率值，而是psate列表中的序号
    arg = OSDynamicCast(OSNumber, getProperty("SpeedID"));
    if(arg==NULL){
        customValue = 0;
    } else {
        customValue = arg->unsigned8BitValue();
    }

    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, powerStates, kNrOfPowerStates);
  
    // 设置了自定义频率并且关闭了cpb
    if(!sleepState.cpb) {
        if(!setCPBState(sleepState.cpb) && ADDPR(debugEnabled))
            IOLog("AMDCPUSupport::start: failed to apply CPBStatus property\n");
        if(customValue>0) {
            setPMPStateLimit(0);
            sleepState.PStateCtl = customValue;
            applyPowerControl(sleepState.PStateCtl);
        } else {
            setPMPStateLimit(1);
        }
    }
    
    return success;
}

void AMDRyzenCPUPowerManagement::stop(IOService *provider){
    pmRyzen_stop();
    
    IOLog("AMDCPUSupport stopped\n");
    
    stopWorkLoop();
    timerEvent_main->cancelTimeout();
    workLoop->removeEventSource(timerEvent_main);
    timerEvent_main->release();
    
    if(superIO){
        for (int i = 0; i < superIO->getNumberOfFans(); i++) {
            superIO->setDefaultFanControl(i);
        }

        delete superIO;
    }

    PMstop();

    IOService::stop(provider);

}

IOReturn AMDRyzenCPUPowerManagement::setPowerState(unsigned long powerStateOrdinal, IOService* provider) {
    if (0 == powerStateOrdinal) {
        // Going to sleep.
        // 0.7.5: do NOT call IOLog here at all — even guarded by
        // ADDPR(debugEnabled), IOLog takes the console output lock,
        // which is a different ticket spinlock from the one that
        // panicked on 2026-08-26 (cpu_lock) but is still a hot global
        // lock while PM is driving 16 CPUs into sleep. Keep the path
        // fully lock-free; rely on `-amdpdbg` logs emitted from the
        // WorkLoop for any forensic trace of sleep/wake.
        sleepState.sleep = true;
        // 0.7.5: removed `getCPBState()` and `dumpPstate()` from the sleep
        // path. Both called `rdmsr_carefully` -> `cpu_lock` while PM was
        // concurrently taking it on other CPUs to drive sleep, causing the
        // ticket spinlock panic on 2026-08-26.
        //
        // sleepState.cpb is kept in sync by setCPBState() on every explicit
        // change, so it always holds the last requested CPB state.
        //
        // The P-state definitions are snapshotted without touching any MSR:
        // PStateDef_perCore is maintained by dumpPstate() (work loop init and
        // the user client writePstate() path), so it already holds the current
        // table — including the user's custom frequency settings. A plain
        // memory copy keeps this path completely lock free.
        stopWorkLoop();
        memcpy(sleepState.pstate, PStateDef_perCore, sizeof(sleepState.pstate));
    } else if (1 == powerStateOrdinal && sleepState.sleep) {
        // Waking up — see sleep branch above for the no-IOLog rationale.
        sleepState.sleep = false;
        restoreWakeState();
        resumeWorkLoop();
    }

    return kIOPMAckImplied;
}

void AMDRyzenCPUPowerManagement::fetchOEMBaseBoardInfo(){
    auto efiRT = EfiRuntimeServices::get();
    uint32_t att = 0;
    uint64_t sizee = BASEBOARD_STRING_MAX;
    uint64_t efistat;
    efistat = efiRT->getVariable(OC_OEM_VENDOR_VARIABLE_NAME, &EfiRuntimeServices::LiluVendorGuid,
                                 &att, &sizee, boardVender);
    
    sizee = BASEBOARD_STRING_MAX;
    efistat = efiRT->getVariable(OC_OEM_BOARD_VARIABLE_NAME, &EfiRuntimeServices::LiluVendorGuid,
                                 &att, &sizee, boardName);
    boardInfoValid = efistat == EFI_SUCCESS;
    IOLog("MB: %s %s\n", boardName, boardVender);
}

bool AMDRyzenCPUPowerManagement::read_msr(uint32_t addr, uint64_t *value){
    
    uint32_t lo, hi;
    //    IOLog("AMDCPUSupport lalala \n");
    int err = rdmsr_carefully(addr, &lo, &hi);
    //    IOLog("AMDCPUSupport rdmsr_carefully %d\n", err);
    
    if(!err) *value = lo | ((uint64_t)hi << 32);
    
    return err == 0;
}

bool AMDRyzenCPUPowerManagement::write_msr(uint32_t addr, uint64_t value){
    if(wrmsr_carefully){
        uint32_t lo = value & 0xffffffff;
        uint32_t hi = value >> 32;
        return (*wrmsr_carefully)(addr, lo, hi) == 0;
    }
    
    //Fall back with unsafe method
    wrmsr64(addr, value);
    
    //If failed, we've already panic and starting reboot. So just return true.
    return true;
}

// 0.7.6: 统一告警入口。
//
// kunc_alert 在 _KUNCUserNotificationDisplayAlert 解析失败时保持 nullptr。
// 以前的 UserClient 在 hasPrivilege() 与 externalMethod() 里各有一处
// 无条件解引用（(*(fProvider->kunc_alert))(...)）——
// 与 cpp:50 的 kextloadAlerts++（符号缺失时每轮自增）组合后，
// 用户态的第一次调用就能把内核带崩。
//
// 现在统一在这里判空：
//   - 弹窗可用：显示弹窗，response 有效，返回 true；
//   - 弹窗不可用：降级为 IOLog，response 置 0，返回 false。
//     "没问到用户" 绝不能被当成 "用户同意了" —— 调用方必须自行降级。
bool AMDRyzenCPUPowerManagement::presentAlert(const char *title, const char *body,
                                               const char *btn1, const char *btn2, const char *btn3,
                                               unsigned int *response){
    // 0.7.6 (Round 2 F4): 哨兵值用 UINT_MAX 而不是 0 —— 0 是一个**合法的按钮下标**
    // （CFUserNotification 的 default 按钮）。用 0 当"没问到用户"的话，
    // 任何忽略 bool 返回值的调用方都会把"没问"误读成"用户按了默认键"。
    if(response) *response = UINT_MAX;
    
    if(!kunc_alert){
        IOLog("AMDCPUSupport: user alert unavailable (%s): %s\n",
              title ? title : "", body ? body : "");
        return false;
    }
    
    (*kunc_alert)(0, 0, NULL, NULL, NULL, title, body, btn1, btn2, btn3, response);
    return true;
}

void AMDRyzenCPUPowerManagement::registerRequest(){
    uint32_t now = (uint32_t)(getCurrentTimeNs() / 1000000);
    
    estimatedRequestTimeInterval = now - timeOfLastMissedRequest;
    timeOfLastMissedRequest = now;
}

// 0.7.6: updateClockSpeed() 已删除 —— 它没有任何调用点（死代码）。
// effFreq_perCore 由 calculateEffectiveFrequency()（APERF/MPERF 差值）维护，
// 不再由 P-state 状态寄存器推导。

void AMDRyzenCPUPowerManagement::calculateEffectiveFrequency(uint8_t physical){
    
    uint32_t APERF_lo, APERF_hi;
    uint32_t MPERF_lo, MPERF_hi;
    
    /**
     * The effective frequency interface provides +/- 50MHz accuracy if the following constraints are met:
     * • Effective frequency is read at most one time per millisecond.
     * • When reading or writing Core::X86::Msr::MPERF and Core::X86::Msr::APERF software executes only
     *  MOV instructions, and no more than 3 MOV instructions, between the two RDMSR or WRMSR
     *  instructions.
     * • Core::X86::Msr::MPERF and Core::X86::Msr::APERF are invalid if an overflow occurs.
    */
    __asm__ volatile("movl $0xe8, %%ecx;"
                     "rdmsr;"
                     "movl %%eax, %0;"
                     "movl %%edx, %1;"
                     "movl $0xe7, %%ecx;"
                     "rdmsr;"
                     : "=r"(APERF_lo), "=r"(APERF_hi), "=a"(MPERF_lo), "=d"(MPERF_hi)
                     :
                     : "%ecx"
                    );
    
    uint64_t APERF = APERF_lo | ((uint64_t)APERF_hi << 32);
    uint64_t MPERF = MPERF_lo | ((uint64_t)MPERF_hi << 32);;

    if(!freqCountersSeeded[physical]){
        // A failed boot-time seed (initWorkLoop) left the baselines at 0;
        // seed from this first successful read instead of reporting one
        // sample scaled from cumulative-since-reset counters.
        lastAPERF_perCore[physical] = APERF;
        lastMPERF_perCore[physical] = MPERF;
        freqCountersSeeded[physical] = true;
        return;
    }
        
    uint64_t lastAPERF = lastAPERF_perCore[physical];
    uint64_t lastMPERF = lastMPERF_perCore[physical];
    
    lastAPERF_perCore[physical] = APERF;
    lastMPERF_perCore[physical] = MPERF;
    //If an overflow of either the MPERF or APERF register occurs between read of last MPERF and
    //read of last APERF, the effective frequency calculated in is invalid.
    if(APERF <= lastAPERF || MPERF <= lastMPERF) {
//        IOLog("AMDCPUSupport::calculateEffectiveFrequency: frequency is invalid!!!");
        lastAPERF_perCore[physical] = APERF;
        lastMPERF_perCore[physical] = MPERF;
        return;
    }
    
    float freqP0 = PStateDefClock_perCore[0];
    
    // A P0 definition we could not read leaves the cached clock at 0. Scaling
    // by it would report a bogus frequency, so keep the last good value.
    if(freqP0 <= 0) return;
    
    uint64_t deltaAPERF = APERF - lastAPERF;
//    float effFreq = ((float)deltaAPERF / (float)(MPERF - lastMPERF)) * freqP0;
    
//    effFreq_perCore[physical] = effFreq;
    uint64_t deltaMPERF = MPERF - lastMPERF;
    if (deltaMPERF != 0) {
        float effFreq = ((float)deltaAPERF / (float)(deltaMPERF)) * freqP0;
        effFreq_perCore[physical] = effFreq;
    }
}

void AMDRyzenCPUPowerManagement::updateInstructionDelta(uint8_t cpu_num){
    uint64_t insCount;
    
    if(!read_msr(kMSR_PERF_IRPC, &insCount))
        return;
    
    
    //Skip if overflowed
    if(lastInstructionDelta_perCore[cpu_num] > insCount) return;
    
//    uint64_t delta = insCount - lastInstructionDelta_perCore[cpu_num];
    instructionDelta_perCore[cpu_num] = insCount - lastInstructionDelta_perCore[cpu_num];
    
    lastInstructionDelta_perCore[cpu_num] = insCount;
    
    //write_msr(kMSR_PERF_IRPC, 0);
    
    
    //Calculate load index
//    float estimatedInstRet = (effFreq_perCore[cpu_num] * 1000000);
//    estimatedInstRet = estimatedInstRet * (actualUpdateTimeInterval * 0.001);
//    float index = (float)delta / estimatedInstRet;
//
//    float growth = 3200;
//    loadIndex_PerCore[cpu_num] = log10f(min(index,1) * growth) / log10f(growth);
}

void AMDRyzenCPUPowerManagement::applyPowerControl(uint8_t pstate){
    struct Args {
        AMDRyzenCPUPowerManagement* provider;
        uint8_t pstate;
    };

    Args args = { this, pstate };

    mp_rendezvous(nullptr, [](void *arg) {
        auto args = static_cast<Args*>(arg);
        auto provider = args->provider;
        auto pstate = args->pstate;

        if(pstate != 255) provider->PStateCtl = pstate;
        provider->sleepState.PStateCtl = provider->PStateCtl;
        provider->write_msr(kMSR_PSTATE_CTL, (uint64_t)(provider->PStateCtl & 0x7));
    }, nullptr, &args);
}
uint8_t AMDRyzenCPUPowerManagement::getPowerControl(){
    uint64_t val;
    read_msr(kMSR_PSTATE_CTL, &val);
    return val & 0x7;
}

bool AMDRyzenCPUPowerManagement::setCPBState(bool enabled){
    if (ADDPR(debugEnabled)) IOLog("AMDCPUSupport::setCPBState enabled is %s\n", enabled?"true":"false");

    if(!cpbSupported) return false;

    uint64_t hwConfig;
    if(!read_msr(kMSR_HWCR, &hwConfig)){
        if (ADDPR(debugEnabled)) IOLog("AMDCPUSupport::setCPBState: failed to read HWCR\n");
        return false;
    }
    
    if(enabled){
        hwConfig &= ~(1 << 25);
    } else {
        hwConfig |= (1 << 25);
    }
    
    struct Args {
        AMDRyzenCPUPowerManagement* provider;
        const uint64_t* hwConfig;
        bool writeOk;
    };

    // Report back whether every CPU accepted the write: only a fully applied
    // change may update the cached state the wake path restores from.
    // (Concurrent false stores to writeOk are identical bytes, so the shared
    // flag cannot tear on x86.)
    Args args = { this, &hwConfig, true };

    mp_rendezvous(nullptr, [](void *arg) {
        auto args = static_cast<Args*>(arg);
        if(!args->provider->write_msr(kMSR_HWCR, *args->hwConfig))
            args->writeOk = false;
    }, nullptr, &args);

    if(!args.writeOk){
        if (ADDPR(debugEnabled)) IOLog("AMDCPUSupport::setCPBState: failed to write HWCR\n");
        return false;
    }

    // Keep the cached CPB state in sync with what was actually requested.
    // restoreWakeState() decides the CpbDis bit from this value, so without
    // this update the wake path would restore the boot-time default instead
    // of the last explicitly chosen state.
    sleepState.cpb = enabled;

    return true;
}

bool AMDRyzenCPUPowerManagement::getCPBState(){
    uint64_t hwConfig;
    if(!read_msr(kMSR_HWCR, &hwConfig)){
        if (ADDPR(debugEnabled)) IOLog("AMDCPUSupport::getCPBState: failed to read HWCR\n");
        // Fall back to the last state we know about. setCPBState() keeps this
        // in sync with every explicit change, so it reflects the last requested
        // state even if the hardware read fails.
        return sleepState.cpb;
    }

    return !((hwConfig >> 25) & 0x1);
}

inline float AMDRyzenCPUPowerManagement::getPackageTemp() {
    IOPCIAddressSpace space;
    space.bits = 0x00;
    
    fIOPCIDevice->configWrite32(space, (UInt8)kFAMILY_17H_PCI_CONTROL_REGISTER, (UInt32)kF17H_M01H_THM_TCON_CUR_TMP);
    uint32_t temperature = fIOPCIDevice->configRead32(space, kFAMILY_17H_PCI_CONTROL_REGISTER + 4);
    
    
    bool tempOffsetFlag = (temperature & kF17H_TEMP_OFFSET_FLAG) != 0;
    temperature = (temperature >> 21) * 125;
    
    float t = temperature * 0.001f;
    
    t -= tempOffset;
    
    if (tempOffsetFlag)
        t -= 49.0f;
    
    return t;
}

void AMDRyzenCPUPowerManagement::updatePackageTemp(){
    float sum = 0;
    for (int i = 0; i < HF_TEMP_SAMPLE_LEN; i++) sum += tempSamples[i];
    PACKAGE_TEMPERATURE_perPackage[0] = sum * HF_TEMP_SAMPLE_LENREP;
}

void AMDRyzenCPUPowerManagement::updatePackageEnergy(){
    
    // 0.7.6: _tscFreq 缺失时 xnuTSCFreq 是默认值（1），除出来的"秒"是
    // 垃圾值，功率读数毫无意义。这种情况下干脆不更新，保留上一次读数。
    // RAPL 单位寄存器读不到时同理（raplValid == false）。
    if(!tscFreqValid || !raplValid) return;
    
    uint64_t ctsc = rdtsc64();

    uint64_t msr_value_buf = 0;
    read_msr(kMSR_PKG_ENERGY_STAT, &msr_value_buf);

    uint32_t energyValue = (uint32_t)(msr_value_buf & 0xffffffff);

    uint64_t energyDelta = (lastUpdateEnergyValue <= energyValue) ?
    energyValue - lastUpdateEnergyValue : UINT32_MAX - lastUpdateEnergyValue;

    double seconds = (ctsc - pwrLastTSC) / (double)(xnuTSCFreq);
    double e = (pwrEnergyUnit * energyDelta) / (seconds);
    e *= pwrTimeUnit * 1000;
    uniPackageEnergy = e;


    lastUpdateEnergyValue = energyValue;
    pwrLastTSC = rdtsc64();
}


bool AMDRyzenCPUPowerManagement::dumpPstate(uint64_t buf[8], bool allowLog){

    bool allOk = true;
    for (uint32_t i = 0; i < kMSR_PSTATE_LEN; i++) {
        uint64_t msr_value_buf = 0;
        if (!read_msr(kMSR_PSTATE_0 + i, &msr_value_buf)) {
            // Never panic on the PM callback path: a failed rdmsr during a
            // sleep/wake transition must not take the whole machine down.
            // Store 0 so writePstate() skips this entry on restore.
            //
            // The cached PStateDef_* entries are deliberately left untouched
            // here: they hold the last known good definition, which is still
            // what the sleep snapshot and the App's P-state table should show.
            // Zeroing them would drop a perfectly valid definition on a
            // transient read failure.
            if (allowLog && ADDPR(debugEnabled)) IOLog("AMDCPUSupport::dumpPstate: failed to read P-state %u\n", i);
            if(buf != nullptr) buf[i] = 0;
            allOk = false;
            continue;
        }
        
        uint32_t eax = (uint32_t)(msr_value_buf & 0xffffffff);
        
        // CpuVid [21:14]
        // CpuDfsId [13:8]
        // CpuFid [7:0]
        int curCpuDfsId = (int)((eax >> 8) & 0x3f);
        int curCpuFid = (int)(eax & 0xff);
        
        float clock = (float)((float)curCpuFid / (float)curCpuDfsId * 200.0);
        
        PStateDef_perCore[i] = msr_value_buf;
        PStateDefClock_perCore[i] = clock;
        if (allowLog && ADDPR(debugEnabled)) IOLog("AMDCPUSupport::dumpPstate: Index: %u, value: %llu\n", i, msr_value_buf);
        if(buf != nullptr) buf[i] = msr_value_buf;
    };

    return allOk;
}

void AMDRyzenCPUPowerManagement::writePstate(const uint64_t *buf){
    
    //A bit hacky but at least works for now.
    void* args[] = {this, (void*)buf};
    
    
    mp_rendezvous(nullptr, [](void *obj) {
        auto v = static_cast<uint64_t*>(((uint64_t**)obj)[1]);
        auto provider = static_cast<AMDRyzenCPUPowerManagement*>(*((AMDRyzenCPUPowerManagement**)obj));

        for (uint32_t i = 0; i < kMSR_PSTATE_LEN; i++) {
            uint64_t def = v[i];
            
            uint64_t curCpuDfsId = ((def >> 8) & 0x3f);
            uint64_t curCpuFid = (def & 0xff);
            if(!def || !curCpuDfsId || !curCpuFid)
                continue;
            
            provider->write_msr(provider->kMSR_PSTATE_0 + i, def);
            
        }
    
    }, nullptr, args);

    // Refresh the cached P-state table on the calling thread, OUTSIDE the
    // rendezvous barrier: dumpPstate -> read_msr takes cpu_lock (and may
    // IOLog under -amdpdbg), which is the exact deadlock/assert hazard the
    // wake path was hardened against. P-state definition MSRs are uniform
    // across the package, so reading them on the calling CPU is equivalent
    // to reading them on the master inside the barrier.
    pstateDumpRetryLeft = 20;
    // allowLog=true is deliberate here: this runs on the user-client thread
    // with interrupts enabled, unlike the rendezvous-internal dumps.
    pstateDumpPending = !dumpPstate(nullptr, true);
        
}

void AMDRyzenCPUPowerManagement::restoreWakeState(){
    struct Args {
        AMDRyzenCPUPowerManagement* provider;
        const uint64_t* pstateBuf;
        uint64_t hwcr;
        uint8_t pstateCtl;
        bool updateHWCR;
        bool anyPstate;
    };

    // Merge what used to be three separate all-core synchronizations per wake
    // (setCPBState / writePstate / applyPowerControl) into a single
    // mp_rendezvous. Each rendezvous requires every CPU to respond, which is a
    // deadlock hazard while cores are still coming back online.

    // Evaluate the snapshot on this thread, before the barrier: if no P-state
    // definition is valid there is nothing to restore, and selecting a
    // P-state index (e.g. P0) without restoring the register file would be
    // worse than leaving the current selection alone.
    bool anyPstate = false;
    for (uint32_t i = 0; i < kMSR_PSTATE_LEN; i++) {
        uint64_t def = sleepState.pstate[i];
        if(def && ((def >> 8) & 0x3f) && (def & 0xff)) anyPstate = true;
    }

    Args args = { this, sleepState.pstate, 0, sleepState.PStateCtl, false, anyPstate };

    if (cpbSupported) {
        // TODO(0.7.6): this read_msr still takes cpu_lock on the master
        // thread while 15 other CPUs are spinning in the rendezvous
        // barrier below. Same shape as the 2026-08-26 panic but in the
        // wake path. Fix by caching HWCR at start() and refreshing only
        // when a deliberate CPB change is requested.
        //
        // 0.7.6 修正：原来这里引用的 "§4 risk #5" 在根因文档中并不存在
        // （该文档没有 §/risk 分节编号），现改为指向确实存在的小节：
        // docs/Task/Archive/2026-08/AMDRyzenCPUPM_SLEEP_RDMSR_ROOTCAUSE.md
        //   §真正的根因（rdmsr_carefully → cpu_lock 抢占）
        //   §仍存在的不确定性
        // 实施方案见 docs/Task/Active/AMDRyzenCPUPM_076_PM_PATH_OPTIMIZATION_PLAN.md
        // 的 T4（HWCR 缓存化）。
        uint64_t hwcr;
        if (read_msr(kMSR_HWCR, &hwcr)) {
            if (sleepState.cpb) {
                hwcr &= ~((uint64_t)1 << 25);
            } else {
                hwcr |= ((uint64_t)1 << 25);
            }
            args.hwcr = hwcr;
            args.updateHWCR = true;
        } else if (ADDPR(debugEnabled)) {
            IOLog("AMDCPUSupport::restoreWakeState: failed to read HWCR, skipping CPB restore\n");
        }
    }

    mp_rendezvous(nullptr, [](void *arg) {
        auto args = static_cast<Args*>(arg);
        auto provider = args->provider;

        if (args->updateHWCR)
            provider->write_msr(kMSR_HWCR, args->hwcr);

        for (uint32_t i = 0; i < kMSR_PSTATE_LEN; i++) {
            uint64_t def = args->pstateBuf[i];

            uint64_t curCpuDfsId = ((def >> 8) & 0x3f);
            uint64_t curCpuFid = (def & 0xff);
            if(!def || !curCpuDfsId || !curCpuFid)
                continue;

            provider->write_msr(provider->kMSR_PSTATE_0 + i, def);
        }

        if(args->anyPstate)
            provider->write_msr(kMSR_PSTATE_CTL, (uint64_t)(args->pstateCtl & 0x7));

        // 0.7.5: removed the master-side `dumpPstate(nullptr)` call here.
        // dumpPstate -> read_msr takes cpu_lock on the calling thread, which
        // deadlocked under the same conditions as the sleep-path panic.
        // PStateDef_* refresh now happens in initWorkLoop()'s first
        // mp_rendezvous_no_intrs (next timer fire, < 200 ms window).
    }, nullptr, &args);
}

bool AMDRyzenCPUPowerManagement::initSuperIO(uint16_t *chipIntel){
    
    superIO = nullptr;
    if(!superIO) superIO = ISSuperIONCT668X::getDevice(&savedSMCChipIntel);
    if(!superIO) superIO = ISSuperIONCT67XXFamily::getDevice(&savedSMCChipIntel);
    if(!superIO) superIO = ISSuperIOIT86XXEFamily::getDevice(&savedSMCChipIntel);
    
    *chipIntel = savedSMCChipIntel;
    
    return superIO != nullptr;
}

uint32_t AMDRyzenCPUPowerManagement::getPMPStateLimit(){
    return pmRyzen_pstatelimit;
}

void AMDRyzenCPUPowerManagement::setPMPStateLimit(uint32_t state){
    pmRyzen_pstatelimit = min(2, state);
    if(state > 0){
        pmRyzen_PState_reset();
    }
}

uint32_t AMDRyzenCPUPowerManagement::getHPcpus(){
    return pmRyzen_hpcpus;
}

EXPORT extern "C" kern_return_t amdryzencpupm_kern_start(kmod_info_t *, void *) {
    // Report success but actually do not start and let I/O Kit unload us.
    // This works better and increases boot speed in some cases.
    PE_parse_boot_argn("liludelay", &ADDPR(debugPrintDelay), sizeof(ADDPR(debugPrintDelay)));
    ADDPR(debugEnabled) = checkKernelArgument("-amdpdbg");
    
//    IOLog("AMDCPUSupport::enter dlinking..\n");
//
//
//    pmRyzen_symtable._wrmsr_carefully = lookup_symbol("_wrmsr_carefully");
//    pmRyzen_symtable._KUNCUserNotificationDisplayAlert = lookup_symbol("_KUNCUserNotificationDisplayAlert");
//    pmRyzen_symtable._cpu_to_processor = lookup_symbol("_cpu_to_processor");
//    pmRyzen_symtable._tscFreq = lookup_symbol("_tscFreq");
//    pmRyzen_symtable._pmDispatch = lookup_symbol("_pmDispatch");
//    pmRyzen_symtable._pmUnRegister = lookup_symbol("_pmUnRegister");
//    pmRyzen_symtable._cpu_NMI_interrupt = lookup_symbol("_cpu_NMI_interrupt");
//    pmRyzen_symtable._NMIPI_enable = lookup_symbol("_NMIPI_enable");
//    pmRyzen_symtable._i386_cpu_IPI = lookup_symbol("_i386_cpu_IPI");
//    IOLog("AMDCPUSupport::enter link finished.\n");
    
    return KERN_SUCCESS;
}

EXPORT extern "C" kern_return_t amdryzencpupm_kern_stop(kmod_info_t *, void *) {
    // It is not safe to unload VirtualSMC plugins!
    return KERN_FAILURE;
}



