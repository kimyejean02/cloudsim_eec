
machine class:
{
        Number of machines: 8
        CPU type: X86
        Number of cores: 8
        Memory: 16384
        S-States: [120, 100, 100, 80, 40, 10, 0]
        P-States: [12, 8, 6, 4]
        C-States: [12, 3, 1, 0]
        MIPS: [3000, 2400, 2000, 1500]
        GPUs: no
}

machine class:
{
        Number of machines: 8
        CPU type: X86
        Number of cores: 8
        Memory: 16384
        S-States: [180, 150, 150, 120, 100, 40, 0]
        P-States: [48, 36, 30, 12]
        C-States: [48, 12, 8, 0]
        MIPS: [3000, 2400, 2000, 1500]
        GPUs: yes
}

machine class:
{
        Number of machines: 8
        CPU type: X86
        Number of cores: 4
        Memory: 8192
        S-States: [40, 20, 16, 12, 10, 4, 0]
        P-States: [4, 2, 2, 1]
        C-States: [4, 1, 1, 0]
        MIPS: [1500, 1200, 1000, 600]
        GPUs: no
}

machine class:
{
        Number of machines: 12
        CPU type: ARM
        Number of cores: 8
        Memory: 16384
        S-States: [80, 40, 28, 20, 12, 8, 0]
        P-States: [8, 4, 2, 1]
        C-States: [8, 2, 1, 0]
        MIPS: [2000, 1500, 1200, 800]
        GPUs: yes
}

machine class:
{
        Number of machines: 2
        CPU type: POWER
        Number of cores: 32
        Memory: 131072
        S-States: [120, 60, 30, 15, 8, 4, 0]
        P-States: [8, 4, 2, 1]
        C-States: [8, 2, 1, 0]
        MIPS: [1500, 1200, 1000, 800]
        GPUs: no
}

# No task ever needs RISCV: these should stay powered off
machine class:
{
        Number of machines: 4
        CPU type: RISCV
        Number of cores: 8
        Memory: 8192
        S-States: [60, 30, 24, 16, 10, 4, 0]
        P-States: [6, 3, 2, 1]
        C-States: [6, 2, 1, 0]
        MIPS: [1200, 900, 700, 500]
        GPUs: no
}

# Phase 1: Low activity
task class:
{
        Start time: 60000
        End time : 8000000
        Inter arrival: 180000
        Expected runtime: 1000000
        Memory: 8
        VM type: LINUX
        GPU enabled: no
        SLA type: SLA2
        CPU type: X86
        Task type: WEB
        Seed: 520230
}

task class:
{
        Start time: 60000
        End time : 8000000
        Inter arrival: 1800000
        Expected runtime: 1000000
        Memory: 8
        VM type: LINUX
        GPU enabled: no
        SLA type: SLA2
        CPU type: ARM
        Task type: WEB
        Seed: 520239
}

# Background: streaming on ARM
task class:
{
        Start time: 5000000
        End time : 55000000
        Inter arrival: 2500000
        Expected runtime: 10000000
        Memory: 512
        VM type: LINUX
        GPU enabled: no
        SLA type: SLA1
        CPU type: ARM
        Task type: STREAM
        Seed: 520240
}

# Phase 2: Burst of urgent tasks
task class:
{
        Start time: 10000000
        End time : 12000000
        Inter arrival: 6000
        Expected runtime: 3000000
        Memory: 32
        VM type: LINUX_RT
        GPU enabled: no
        SLA type: SLA0
        CPU type: X86
        Task type: WEB
        Seed: 520120
}

# Windows burst competing for the same X86 machines
task class:
{
        Start time: 10500000
        End time : 11500000
        Inter arrival: 20000
        Expected runtime: 2000000
        Memory: 64
        VM type: WIN
        GPU enabled: no
        SLA type: SLA1
        CPU type: X86
        Task type: CRYPTO
        Seed: 520122
}

# Phase 3: GPU-intensive workload
task class:
{
        Start time: 20000000
        End time : 35000000
        Inter arrival: 50000
        Expected runtime: 1000000
        Memory: 2048
        VM type: LINUX
        GPU enabled: yes
        SLA type: SLA1
        CPU type: X86
        Task type: AI
        Seed: 520121
}

task class:
{
        Start time: 20000000
        End time : 35000000
        Inter arrival: 200000
        Expected runtime: 1500000
        Memory: 2048
        VM type: LINUX
        GPU enabled: yes
        SLA type: SLA1
        CPU type: ARM
        Task type: AI
        Seed: 520123
}

# Memory-heavy, no GPU: belongs on the small X86 machines, not the GPU ones
task class:
{
        Start time: 22000000
        End time : 32000000
        Inter arrival: 300000
        Expected runtime: 2000000
        Memory: 4096
        VM type: LINUX
        GPU enabled: no
        SLA type: SLA2
        CPU type: X86
        Task type: CRYPTO
        Seed: 520124
}

# Phase 4: Long-running best-effort workload
task class:
{
        Start time: 5000000
        End time : 50000000
        Inter arrival: 5000000
        Expected runtime: 20000000
        Memory: 16384
        VM type: AIX
        GPU enabled: no
        SLA type: SLA3
        CPU type: POWER
        Task type: HPC
        Seed: 520231
}

# Phase 5: Return to low activity
task class:
{
        Start time: 40000000
        End time : 60000000
        Inter arrival: 180000
        Expected runtime: 1000000
        Memory: 8
        VM type: LINUX
        GPU enabled: no
        SLA type: SLA2
        CPU type: X86
        Task type: WEB
        Seed: 520232
}

task class:
{
        Start time: 40000000
        End time : 60000000
        Inter arrival: 1800000
        Expected runtime: 1000000
        Memory: 8
        VM type: LINUX
        GPU enabled: no
        SLA type: SLA2
        CPU type: ARM
        Task type: WEB
        Seed: 520233
}
