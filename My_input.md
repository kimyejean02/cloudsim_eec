
machine class:
{
        Number of machines: 8
        CPU type: X86
        Number of cores: 8
        Memory: 16384
        S-States: [120, 100, 100, 80, 40, 10, 0]
        P-States: [12, 8, 6, 4]
        C-States: [12, 3, 1, 0]
        MIPS: [1000, 800, 600, 400]
        GPUs: yes
}

machine class:
{
        Number of machines: 8
        Number of cores: 4
        CPU type: X86
        Memory: 8192
        S-States: [40, 20, 16, 12, 10, 4, 0]
        P-States: [4, 2, 2, 1]
        C-States: [4, 1, 1, 0]
        MIPS: [700, 550, 400, 250]
        GPUs: no
}

machine class:
{
        Number of machines: 12
        Number of cores: 16
        CPU type: ARM
        Memory: 16384
        S-States: [80, 40, 28, 20, 12, 8, 0]
        P-States: [8, 4, 2, 1]
        C-States: [8, 2, 1, 0]
        MIPS: [1000, 800, 600, 400]
        GPUs: yes
}

machine class:
{
        Number of machines: 2
        Number of cores: 32
        CPU type: POWER
        Memory: 131072
        S-States: [120, 60, 30, 15, 8, 4, 0]
        P-States: [8, 4, 2, 1]
        C-States: [8, 2, 1, 0]
        MIPS: [1500, 1200, 1000, 800]
        GPUs: no
}

# Phase 1: Low activity
task class:
{
        Start time: 60000
        End time: 8000000
        Inter arrival: 1500000
        Expected runtime: 300000
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
        End time: 8000000
        Inter arrival: 2000000
        Expected runtime: 400000
        Memory: 8
        VM type: LINUX
        GPU enabled: no
        SLA type: SLA2
        CPU type: ARM
        Task type: WEB
        Seed: 520239
}

# Phase 2: Burst of urgent tasks
task class:
{
        Start time: 10000000
        End time: 12000000
        Inter arrival: 50000
        Expected runtime: 500000
        Memory: 32
        VM type: LINUX_RT
        GPU enabled: no
        SLA type: SLA0
        CPU type: X86
        Task type: WEB
        Seed: 520120
}

# Phase 3: GPU-intensive workload
task class:
{
        Start time: 20000000
        End time: 35000000
        Inter arrival: 150000
        Expected runtime: 1200000
        Memory: 1024
        VM type: LINUX
        GPU enabled: yes
        SLA type: SLA1
        CPU type: X86
        Task type: AI
        Seed: 520121
}

# Phase 4: Long-running best-effort workload
task class:
{
        Start time: 5000000
        End time: 50000000
        Inter arrival: 15000000
        Expected runtime: 20000000
        Memory: 8192
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
        End time: 60000000
        Inter arrival: 1500000
        Expected runtime: 300000
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
        End time: 60000000
        Inter arrival: 2000000
        Expected runtime: 400000
        Memory: 8
        VM type: LINUX
        GPU enabled: no
        SLA type: SLA2
        CPU type: ARM
        Task type: WEB
        Seed: 520233
}
