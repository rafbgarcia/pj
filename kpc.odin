package main

import "core:time"
import "core:fmt"
import "core:dynlib"

KPC_MAX_COUNTERS :: 32

// Event names from /usr/share/kpep/<cpu>.plist. kperfdata resolves the
// encodings and counter slots for the current CPU.
EVENTS :: [?]cstring{
    "FIXED_CYCLES",
    "FIXED_INSTRUCTIONS",
    "L1D_CACHE_MISS_LD",
    "L1D_TLB_MISS",
    "L2_TLB_MISS_DATA",
    "MMU_TABLE_WALK_DATA",
    "PL2_CACHE_MISS_LD",
}
EVENT_LABELS :: [?]string{"cycles", "instrs", "l1d_miss_ld", "l1d_tlb_miss", "l2_tlb_miss", "tlb_walk", "l2_miss_ld"}
N_EVENTS :: len(EVENTS)

Counters :: [N_EVENTS]u64

Kpc :: struct {
    force_all_ctrs_set:  proc "c" (val: i32) -> i32,
    set_config:          proc "c" (classes: u32, config: [^]u64) -> i32,
    set_counting:        proc "c" (classes: u32) -> i32,
    set_thread_counting: proc "c" (classes: u32) -> i32,
    get_thread_counters: proc "c" (tid: u32, count: u32, buf: [^]u64) -> i32,
    counter_map:         [KPC_MAX_COUNTERS]uint,
}
kpc: Kpc

Kpep :: struct {
    db_create:             proc "c" (name: cstring, db: ^rawptr) -> i32,
    db_event:              proc "c" (db: rawptr, name: cstring, ev: ^rawptr) -> i32,
    config_create:         proc "c" (db: rawptr, cfg: ^rawptr) -> i32,
    config_force_counters: proc "c" (cfg: rawptr) -> i32,
    config_add_event:      proc "c" (cfg: rawptr, ev: ^rawptr, flag: u32, err: ^u32) -> i32,
    config_kpc_classes:    proc "c" (cfg: rawptr, classes: ^u32) -> i32,
    config_kpc_count:      proc "c" (cfg: rawptr, count: ^uint) -> i32,
    config_kpc_map:        proc "c" (cfg: rawptr, buf: [^]uint, size: uint) -> i32,
    config_kpc:            proc "c" (cfg: rawptr, buf: [^]u64, size: uint) -> i32,
}

kpc_init :: proc() -> bool {
    lib, ok := dynlib.load_library("/System/Library/PrivateFrameworks/kperf.framework/kperf")
    if !ok do return false
    kpc.force_all_ctrs_set  = auto_cast dynlib.symbol_address(lib, "kpc_force_all_ctrs_set")
    kpc.set_config          = auto_cast dynlib.symbol_address(lib, "kpc_set_config")
    kpc.set_counting        = auto_cast dynlib.symbol_address(lib, "kpc_set_counting")
    kpc.set_thread_counting = auto_cast dynlib.symbol_address(lib, "kpc_set_thread_counting")
    kpc.get_thread_counters = auto_cast dynlib.symbol_address(lib, "kpc_get_thread_counters")

    data_lib, data_ok := dynlib.load_library("/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata")
    if !data_ok do return false
    kpep: Kpep
    kpep.db_create             = auto_cast dynlib.symbol_address(data_lib, "kpep_db_create")
    kpep.db_event              = auto_cast dynlib.symbol_address(data_lib, "kpep_db_event")
    kpep.config_create         = auto_cast dynlib.symbol_address(data_lib, "kpep_config_create")
    kpep.config_force_counters = auto_cast dynlib.symbol_address(data_lib, "kpep_config_force_counters")
    kpep.config_add_event      = auto_cast dynlib.symbol_address(data_lib, "kpep_config_add_event")
    kpep.config_kpc_classes    = auto_cast dynlib.symbol_address(data_lib, "kpep_config_kpc_classes")
    kpep.config_kpc_count      = auto_cast dynlib.symbol_address(data_lib, "kpep_config_kpc_count")
    kpep.config_kpc_map        = auto_cast dynlib.symbol_address(data_lib, "kpep_config_kpc_map")
    kpep.config_kpc            = auto_cast dynlib.symbol_address(data_lib, "kpep_config_kpc")

    if kpc.force_all_ctrs_set(1) != 0 do return false // fails without sudo

    db, cfg: rawptr
    if kpep.db_create(nil, &db) != 0 do return false // nil = current CPU
    if kpep.config_create(db, &cfg) != 0 do return false
    if kpep.config_force_counters(cfg) != 0 do return false

    events := EVENTS
    for name in events {
        ev: rawptr
        if kpep.db_event(db, name, &ev) != 0 {
            fmt.printfln("> kpep: event not found: %s", name)
            return false
        }
        if kpep.config_add_event(cfg, &ev, 0, nil) != 0 {
            fmt.printfln("> kpep: can't add event: %s", name)
            return false
        }
    }

    classes: u32
    reg_count: uint
    regs: [KPC_MAX_COUNTERS]u64
    if kpep.config_kpc_classes(cfg, &classes) != 0 do return false
    if kpep.config_kpc_count(cfg, &reg_count) != 0 do return false
    if kpep.config_kpc_map(cfg, &kpc.counter_map[0], size_of(kpc.counter_map)) != 0 do return false
    if kpep.config_kpc(cfg, &regs[0], size_of(regs)) != 0 do return false

    if reg_count > 0 && kpc.set_config(classes, &regs[0]) != 0 do return false
    if kpc.set_counting(classes) != 0 do return false
    if kpc.set_thread_counting(classes) != 0 do return false
    return true
}

kpc_deinit :: proc() {
    kpc.set_counting(0)
    kpc.set_thread_counting(0)
    kpc.force_all_ctrs_set(0)
}

// returns the counters for EVENTS on the current thread
kpc_read :: proc() -> (out: Counters) {
    buf: [KPC_MAX_COUNTERS]u64
    kpc.get_thread_counters(0, len(buf), &buf[0])
    for i in 0..<N_EVENTS do out[i] = buf[kpc.counter_map[i]]
    return
}


@(deferred_out=cycles_end)
SCOPED_CYCLES :: proc(name: string, items: int) -> (string, int, Counters, time.Tick) {
    c := kpc_read()
    t := time.tick_now()
    return name, items, c, t
}

cycles_end :: proc(name: string, items: int, c0: Counters, t0: time.Tick) {
    c1 := kpc_read()
    secs := time.duration_seconds(time.tick_since(t0))
    d: Counters
    for i in 0..<N_EVENTS do d[i] = c1[i] - c0[i]
    cycles, instructions := d[0], d[1]

    cycles_buf: [32]u8
    fmt.printfln("%s:", name)
    fmt.printfln("  - cycles: %s", group_digits(cycles_buf[:], cycles))
    fmt.printfln("  - cycles/item: %.1f", f64(cycles) / f64(items))
    fmt.printfln("  - instr/item: %.1f", f64(instructions) / f64(items))
    fmt.printfln("  - instrs/cycle: %.2f", f64(instructions) / f64(cycles))
    fmt.printfln("  - secs: %.2f", secs)

    labels := EVENT_LABELS
    for i in 2..<N_EVENTS {
        fmt.printfln("  - %s/item: %.3f", labels[i], f64(d[i]) / f64(items))
    }
    fmt.println()
}

// 56946918411 -> "56,946,918,411"
group_digits :: proc(buf: []u8, n: u64) -> string {
    digits: [20]u8
    s := fmt.bprintf(digits[:], "%d", n)
    j := 0
    for i in 0..<len(s) {
        if i > 0 && (len(s) - i) % 3 == 0 {
            buf[j] = ','
            j += 1
        }
        buf[j] = s[i]
        j += 1
    }
    return string(buf[:j])
}
