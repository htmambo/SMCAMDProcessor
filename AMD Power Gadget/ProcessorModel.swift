//
//  ProcessorModel.swift
//  AMD Power Gadget
//
//  Created by trulyspinach on 3/3/20.
//

import Cocoa


class ProcessorModel {
    static let shared = ProcessorModel()
    
    private var connect: io_connect_t = 0
    
    private var cachedMetric : [Float] = []
    private var numberOfCores : Int = 0
    private var lastMLoad : Double = 0
    
    private var PStateDef : [UInt64] = []
    private var PStateCur : Int = 0
    private var instructionDelta : [UInt64] = []
    private var loadIndex : [Float] = []
    private var PStateDefClock : [Float] = []
    private var vaildPStateLength : Int = 0
    
    private var cpuListedAsSupported : Bool = false
    
    var systemConfig : [String : String] = [:]
    
    var AMDRyzenCPUPowerManagementVersion : String = ""
    var cpuidBasic : [UInt64] = []
    var boardValid = false
    var boardName : String = NSLocalizedString("Unknown", comment: "Placeholder shown when the motherboard name is not available")
    var boardVender : String = NSLocalizedString("Unknown", comment: "Placeholder shown when the motherboard vendor is not available")
    
    var fetchRetry : Int = 10
    var fetchRetry2 : Int = 10
    var retryTimer : Timer?

    /// Whether the shared instance finished initializing.
    /// The termination handler uses this to avoid touching the lazily initialized
    /// singleton, which would re-enter its own dispatch_once while it is still running.
    static var isInitialized : Bool { return initialized }
    private static var initialized = false

    /// Oldest kext version whose user client API this app is able to talk to.
    /// Anything newer than this stays compatible, so bumping the kext version
    /// does not lock the app out.
    private static let minimumCompatibleVersion = "0.6.3"

    /// Newest kext version this app was built and tested against.
    /// Only used to tell the user that a newer kext is available.
    private static let latestKnownVersion = "0.7.6"

    private static func versionComponents(_ version : String) -> [Int] {
        version.split(separator: ".").map { Int($0) ?? 0 }
    }

    private static func compareVersions(_ lhs : String, _ rhs : String) -> ComparisonResult {
        let l = versionComponents(lhs)
        let r = versionComponents(rhs)
        for index in 0..<max(l.count, r.count) {
            let lv = index < l.count ? l[index] : 0
            let rv = index < r.count ? r[index] : 0
            if lv != rv {
                return lv < rv ? .orderedAscending : .orderedDescending
            }
        }
        return .orderedSame
    }

    private static func isCompatibleVersion(_ version : String) -> Bool {
        return compareVersions(version, minimumCompatibleVersion) != .orderedAscending
    }

    init() {
        if !initDriver() {
            alertAndQuit(title: NSLocalizedString("AMDRyzenCPUPowerManagement Not Found",
                                                  comment: "Alert title shown when the AMDRyzenCPUPowerManagement driver is not loaded"),
                         message: NSLocalizedString("Please download AMDRyzenCPUPowerManagement from the release page.",
                                                    comment: "Alert message shown when the AMDRyzenCPUPowerManagement driver is not loaded"))
            return
        }

        var scalerOut: UInt64 = 0
        var outputCount: UInt32 = 0

        let maxStrLength = 16
        var outputStr: [CChar] = [CChar](repeating: 0, count: maxStrLength)
        var outputStrCount: Int = maxStrLength
        let _ = IOConnectCallMethod(connect, 8, nil, 0, nil, 0,
                                      &scalerOut, &outputCount,
                                      &outputStr, &outputStrCount)
        AMDRyzenCPUPowerManagementVersion = String(cString: Array(outputStr[0...outputStrCount-1]))
        
        // The app talks to the kext through its user client API, which only changes when
        // the kext makes a breaking change. Comparing versions numerically means bumping
        // the kext version no longer locks the app out.
        if !Self.isCompatibleVersion(AMDRyzenCPUPowerManagementVersion) {
            alertAndQuit(title: NSLocalizedString("AMDRyzenCPUPowerManagement Version Incompatible",
                                                  comment: "Alert title shown when the loaded driver version is too old"),
                         message: String(format: NSLocalizedString("Your AMDRyzenCPUPowerManagement version is %@, which is outdated and no longer API compatible. Please use the latest version and start this application again.",
                                                                 comment: "Alert message shown when the loaded driver version is too old"),
                                         AMDRyzenCPUPowerManagementVersion))
            return
        }
        
        if Self.compareVersions(AMDRyzenCPUPowerManagementVersion, Self.latestKnownVersion) == .orderedAscending {
            alertDontQuit(message: NSLocalizedString("There are updates available for AMDRyzenCPUPowerManagement. Update to the latest version for more feature and stability.",
                                                     comment: "Alert message shown when a newer driver version exists"))
        }
        
        loadCPUID()
        loadBaseBoardInfo()
        loadMetric()
        loadSystemConfig()
        loadPStateDef()
        loadPStateDefClock()
        
        
        if numberOfCores < 1{
            let alert = NSAlert()
            alert.messageText = NSLocalizedString("Error reading CPU data.", comment: "Alert title shown when the driver reports incorrect CPU data")
            alert.informativeText = NSLocalizedString("This application can not be launched due to AMDRyzenCPUPowerManagement is reporting incorrect data.", comment: "Alert message shown when the driver reports incorrect CPU data")
            alert.alertStyle = .critical
            alert.addButton(withTitle: NSLocalizedString("Quit", comment: "Button title to quit the application"))
            alert.runModal()
            NSApplication.shared.terminate(self)
        }
        
//        fetchSupportedProcessor()
        Self.initialized = true
    }
    
    func initDriver() -> Bool {
        let serviceObject = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                        IOServiceMatching("AMDRyzenCPUPowerManagement"))
        if serviceObject == 0 {
            return false
        }
        
        let status = IOServiceOpen(serviceObject, mach_task_self_, 0, &connect)
        print(status)
        
        return status == KERN_SUCCESS
    }
    
    func closeDriver() {
        if connect == 0 { return }
        IOServiceClose(connect)
        connect = 0
    }
    
    func alertAndQuit(title : String, message : String){
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.alertStyle = .critical
        alert.addButton(withTitle: NSLocalizedString("Quit", comment: "Button title to quit the application"))
        alert.addButton(withTitle: NSLocalizedString("Quit and Download", comment: "Button title to quit the application and open the download page"))
        let res = alert.runModal()
        
        if res == .alertSecondButtonReturn {
            NSWorkspace.shared.open(URL(string: "https://github.com/trulyspinach/SMCAMDProcessor")!)
        }
        
        NSApplication.shared.terminate(self)
    }
    
    func alertDontQuit(message : String){
        let alert = NSAlert()
        alert.messageText = NSLocalizedString("Kext Update Available", comment: "Alert title shown when a newer driver version exists")
        alert.informativeText = message
        alert.alertStyle = .warning
        alert.addButton(withTitle: NSLocalizedString("Later", comment: "Button title to dismiss the update alert"))
        alert.addButton(withTitle: NSLocalizedString("Download", comment: "Button title to open the download page"))
        let res = alert.runModal()
        
        if res == .alertSecondButtonReturn {
            NSWorkspace.shared.open(URL(string: "https://github.com/trulyspinach/SMCAMDProcessor")!)
        }
    }
    
    func kernelGetFloats(count : Int, selector : UInt32) -> [Float] {
        var scalerOut: UInt64 = 0
        var outputCount: UInt32 = 1

        let maxStrLength = count //MaxCpu + 3
        var outputStr: [Float] = [Float](repeating: 0, count: maxStrLength)
        var outputStrCount: Int = 4/*sizeof(float)*/ * maxStrLength
        let res = IOConnectCallMethod(connect, selector, nil, 0, nil, 0,
                                      &scalerOut, &outputCount,
                                      &outputStr, &outputStrCount)
        
        if res != KERN_SUCCESS {
            print(String(cString: mach_error_string(res)))
            return []
        }
        
        return outputStr
    }
    
    func kernelGetUInt64(count : Int, selector : UInt32) -> [UInt64] {
        var scalerOut: UInt64 = 0
        var outputCount: UInt32 = 1

        let maxStrLength = count //MaxCpu + 3
        var outputStr: [UInt64] = [UInt64](repeating: 0, count: maxStrLength)
        var outputStrCount: Int = 8/*sizeof(uint64_t)*/ * maxStrLength
        let res = IOConnectCallMethod(connect, selector, nil, 0, nil, 0,
                                      &scalerOut, &outputCount,
                                      &outputStr, &outputStrCount)
        
        if res != KERN_SUCCESS {
            print(String(cString: mach_error_string(res)))
            return []
        }
        
        return outputStr
    }
    
    func kernelGetString(selector : UInt32, args : [UInt64]) -> String {
        
        var argcpy = args
        var outbuffersize = 16
        var outputStr: [CChar] = [CChar](repeating: 0, count: outbuffersize)
        
        var res = IOConnectCallMethod(connect, selector, &argcpy, UInt32(args.count), nil, 0,
                                      nil, nil,
                                      &outputStr, &outbuffersize)
        
        if res == MIG_ARRAY_TOO_LARGE{
            outputStr = [CChar](repeating: 0, count: outbuffersize)
            res = IOConnectCallMethod(connect, selector, &argcpy, UInt32(args.count), nil, 0,
                                      nil, nil,
                                      &outputStr, &outbuffersize)
        }
        else if res != KERN_SUCCESS {
            print(String(cString: mach_error_string(res)))
            return ""
        }
        
        
        return String(String(cString: Array(outputStr[0...outbuffersize-1])).prefix(outbuffersize))
    }
    
    func kernelSetUInt64(selector : UInt32, args : [UInt64]) -> Bool {
        var argcpy = args
        let res = IOConnectCallMethod(connect, selector, &argcpy, UInt32(args.count), nil, 0,
                                      nil, nil,
                                      nil, nil)
        
        return res == KERN_SUCCESS
    }
    
    private func loadMetric(){
        var scalerOut: UInt64 = 0
        var outputCount: UInt32 = 1

        let maxStrLength = 67 //MaxCpu + 3
        var outputStr: [Float] = [Float](repeating: 0, count: maxStrLength)
        var outputStrCount: Int = 4/*sizeof(float)*/ * maxStrLength
        let res = IOConnectCallMethod(connect, 4, nil, 0, nil, 0,
                                      &scalerOut, &outputCount,
                                      &outputStr, &outputStrCount)
        
        if res != KERN_SUCCESS {
            print(String(cString: mach_error_string(res)))
            return
        }
        
        numberOfCores = Int(scalerOut)
        cachedMetric = Array(outputStr[0...numberOfCores + 2])
        PStateCur = Int(outputStr[2])
        
        
        lastMLoad = NSDate().timeIntervalSince1970
    }
    
    private func loadLoadIndex(){
        let o = kernelGetFloats(count: numberOfCores, selector: 6)
        loadIndex = Array(o[0...numberOfCores - 1])
    }
    
    private func loadPStateDef(){

        PStateDef = kernelGetUInt64(count: 8, selector: 0)
        print(PStateDef)
        var i = 0
        while i < 8 {
            if (PStateDef[i] & 0x8000000000000000) == 0 { //LOL Swift
                break
            }
            i += 1
        }
        vaildPStateLength = i
        
    }
    
    private func loadCPUID(){
        cpuidBasic = kernelGetUInt64(count: 8, selector: 7)
    }
    
    private func loadBaseBoardInfo(){
        var scalerOut: [UInt64] = [UInt64](repeating: 0, count: 1)
        var outputCount: UInt32 = 1

        let maxStrLength = 128
        var outputStr: [CChar] = [CChar](repeating: 0, count: maxStrLength)
        var outputStrCount: Int = maxStrLength
        let _ = IOConnectCallMethod(connect, 16, nil, 0, nil, 0,
                                      &scalerOut, &outputCount,
                                      &outputStr, &outputStrCount)
        
        if scalerOut[0] == 1 {
            boardValid = true
            boardVender = String(cString: Array(outputStr[0...64-1]))
                .trimmingCharacters(in: .whitespacesAndNewlines)
                .trimmingCharacters(in: .controlCharacters)
            boardName = String(cString: Array(outputStr[64...128-1]))
                .trimmingCharacters(in: .whitespacesAndNewlines)
                .trimmingCharacters(in: .controlCharacters)
        }
        
    }
    
    private func loadPStateDefClock(){
        PStateDefClock = kernelGetFloats(count: 10, selector: 1)
    }
    
    func refreshPStateDef() {
        loadPStateDefClock()
    }
    
    func getHPCpus() -> Int{
        let o = kernelGetUInt64(count: 1, selector: 17)
        return Int(o[0])
    }
    
    func setPState(state : Int) {
        var input: [UInt64] = [UInt64(state)]
        let res = IOConnectCallMethod(connect, 10, &input, 1, nil, 0,
                                      nil, nil,
                                      nil, nil)
        
        if res != KERN_SUCCESS {
            print(String(cString: mach_error_string(res)))
            return
        }
    }
    
    func getPState() -> Int {
        return PStateCur
    }
    
    func getPStateDef() -> [UInt64]{
        return PStateDef
    }
    
    func getVaildPStateClocks() -> [Float] {
        return Array(PStateDefClock[0...vaildPStateLength-1])
    }
    
    func getMetric(forced : Bool) -> [Float] {
        if forced || (NSDate().timeIntervalSince1970 - lastMLoad >= 1.0) {
            loadMetric()
        }
        return cachedMetric
    }
    
    func getNumOfCore() -> Int {
        return numberOfCores
    }
    
    func getLoadIndex() -> [Float] {
        loadLoadIndex()
        return loadIndex
    }
    
    func getCPB() -> [Bool] {
        let o = kernelGetUInt64(count: 2, selector: 11)
        return o.map{ $0 == 0 ? false : true }
    }
    
    func setCPB(enabled : Bool){
        var input: [UInt64] = [UInt64(enabled ? 1 : 0)]
        let res = IOConnectCallMethod(connect, 12, &input, 1, nil, 0,
                                      nil, nil,
                                      nil, nil)
        
        if res != KERN_SUCCESS {
            print(String(cString: mach_error_string(res)))
            return
        }
    }
    
    func getPPM() -> Bool {
        let o = kernelGetUInt64(count: 2, selector: 13)
        return o[0] == 0 ? false : true
    }
    
    func setPPM(enabled : Bool){
        var input: [UInt64] = [UInt64(enabled ? 1 : 0)]
        let res = IOConnectCallMethod(connect, 14, &input, 1, nil, 0,
                                      nil, nil,
                                      nil, nil)
        
        if res != KERN_SUCCESS {
            print(String(cString: mach_error_string(res)))
            return
        }
    }
    
    func getLPM() -> Bool {
        let o = kernelGetUInt64(count: 1, selector: 18)
        return o[0] == 0 ? false : true
    }
    
    func setLPM(enabled : Bool){
        var input: [UInt64] = [UInt64(enabled ? 1 : 0)]
        let res = IOConnectCallMethod(connect, 19, &input, 1, nil, 0,
                                      nil, nil,
                                      nil, nil)
        
        if res != KERN_SUCCESS {
            print(String(cString: mach_error_string(res)))
            return
        }
    }
    
    func getInstructionDelta() -> [UInt64]{
        let o = kernelGetUInt64(count: 1, selector: 5)
        return [o[0]]
    }
    
    func setPState(def : [UInt64]) -> Int{
        if def.count != 8 {
            return -1
        }
        
        var input: [UInt64] = def
        let res = IOConnectCallMethod(connect, 15, &input, 8, nil, 0,
                                      nil, nil,
                                      nil, nil)
        
        
        if res != KERN_SUCCESS {
            print(String(cString: mach_error_string(res)))
            return Int(res)
        }
        
        loadPStateDef()
        loadPStateDefClock()
        return 0
    }
    
    static func sysctlString(key : String) -> String {
        var size = 0
        sysctlbyname(key, nil, &size, nil, 0)
        var machine = [CChar](repeating: 0,  count: size)
        sysctlbyname(key, &machine, &size, nil, 0)
        return String(cString: machine)
    }
    
    static func sysctlInt64(key : String) -> Int64 {
        var v: Int64 = 0
        var size = MemoryLayout<Int64>.size
        sysctlbyname(key, &v, &size, nil, 0)
        return v
    }
    
    func loadSystemConfig() {
        systemConfig["ver"] = AMDRyzenCPUPowerManagementVersion
        systemConfig["cpu"] = ProcessorModel.sysctlString(key: "machdep.cpu.brand_string")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        systemConfig["os"] = ProcessorModel.sysctlString(key: "kern.osproductversion")
        systemConfig["mem"] = "\(Int(ProcessorModel.sysctlInt64(key: "hw.memsize") / 1024 / 1024))"
        
        
        let paths = NSSearchPathForDirectoriesInDomains(.documentDirectory, .userDomainMask, true)
        if let dictionary = try? FileManager.default.attributesOfFileSystem(forPath: paths.last!) {
            if let size = dictionary[FileAttributeKey.systemSize] as? NSNumber {
                systemConfig["rs"] = "\(Int(Int(truncating: size) / 1024 / 1024))"
            }
        }
        
        if boardValid {
            systemConfig["mb"] = "\(boardName) \(boardVender)"
        }
        
        var iter : io_iterator_t = 0
        let err = IOServiceGetMatchingServices(kIOMasterPortDefault,
                                               IOServiceMatching("IOPCIDevice"), &iter)
        if err != kIOReturnSuccess {return}
        while true {
            let reg = IOIteratorNext(iter)
            if reg == 0 { break}
            var serviceDictionary : Unmanaged<CFMutableDictionary>?
            let e = IORegistryEntryCreateCFProperties(reg, &serviceDictionary, kCFAllocatorDefault, .zero)
            
            if e != kIOReturnSuccess {continue}
            if let dic : NSDictionary = serviceDictionary?.takeRetainedValue(){
                if let type = dic.object(forKey: "IOName") as? String {
                    if type != "display" {continue}
                    
                    if let model = dic.object(forKey: "model") as? Data {
                        systemConfig["gpu"] = String(data: model, encoding: .ascii)!
                            .trimmingCharacters(in: .controlCharacters)
                            .trimmingCharacters(in: .whitespacesAndNewlines)
                    } else {
                        systemConfig["gpu"] = NSLocalizedString("Unknown", comment: "Placeholder shown when the GPU model is not available")
                    }
                }
            }
        }
    }
    
    func fetchSupportedProcessor() {
        let url = URL(string: "https://bot1.spinach.wtf/chksupport")

        guard let requestUrl = url else { fatalError() }

        var request = URLRequest(url: requestUrl)
        request.httpMethod = "POST"
        
        
        let postString = systemConfig.reduce(into: "") { (r, arg1) in
            let (key, value) = arg1
            r += "&\(key)=\(value)"
        }

        request.httpBody = postString.data(using: String.Encoding.utf8)
        let task = URLSession.shared.dataTask(with: request) { (data, _, error) in
            if error != nil {
                if self.fetchRetry > 0 {
                    self.fetchRetry -= 1

                    DispatchQueue.main.asyncAfter(deadline: .now() + 8.0, execute: {
                        self.fetchSupportedProcessor()
                    })
                }
                return
            }
         
            
            if let data = data, let dataString = String(data: data, encoding: .utf8) {
                self.cpuListedAsSupported = dataString == "true"
            }
        }
        task.resume()
        
    }
    
    func fetchSMCChipSupport(chipIntel : Int, working : Bool) {
        let url = URL(string: "https://bot1.spinach.wtf/chksmcchipsupport")

        guard let requestUrl = url else { fatalError() }

        var request = URLRequest(url: requestUrl)
        request.httpMethod = "POST"
        
        var smcConfig = systemConfig
        smcConfig["smc"] = String(format: "%X", chipIntel)
        smcConfig["smcloaded"] = working ? "Yes" : "No"
        
        let postString = smcConfig.reduce(into: "") { (r, arg1) in
            let (key, value) = arg1
            r += "&\(key)=\(value)"
        }

        request.httpBody = postString.data(using: String.Encoding.utf8)
        let task = URLSession.shared.dataTask(with: request) { (_, _, error) in
            if error != nil {
                if self.fetchRetry2 > 0 {
                    self.fetchRetry2 -= 1

                    DispatchQueue.main.asyncAfter(deadline: .now() + 8.0, execute: {
                        self.fetchSMCChipSupport(chipIntel: chipIntel, working: working)
                    })
                }
                return
            }
         
            
//            if let data = data, let dataString = String(data: data, encoding: .utf8) {
//
//            }
        }
        task.resume()
    }
}
