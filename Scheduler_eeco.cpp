#include <cstdint>
#include <algorithm>
#include "Scheduler.hpp"

// Machine tiers for each CPU type:
//   running - S0, hosting tasks
//   standby - light sleep, fast to wake up
//   off     - deep sleep, slow to wake up
#define STANDBY_STATE   S1          // S1 wakes in about 0.3 seconds
#define OFF_STATE       S4          // S5 takes about 5 minutes to wake up
#define MIN_RUNNING     1
#define STANDBY_SHARE   0.25
#define UP_THRESHOLD    0.8
#define DOWN_THRESHOLD  0.5
#define IDLE_WAIT       1000000
#define CHECK_INTERVAL  100000

typedef enum {
    RUNNING,
    WAKING,
    STANDBY,
    GOING_STANDBY,
    OFF,
    GOING_OFF
} Tier_t;

static unsigned active_machines = 0;
static vector<Tier_t> tier;
static vector<Time_t> last_busy;
static vector<TaskId_t> pending;
static unsigned type_count[4] = {0, 0, 0, 0};
static Time_t last_check = 0;

static double Efficiency(MachineId_t machine_id) {
    MachineInfo_t machine = Machine_GetInfo(machine_id);
    double mips = double(machine.performance[0]) * machine.num_cpus;
    double power = machine.s_states[0] +
                   double(machine.p_states[0]) * machine.num_cpus;
    return mips / power;
}

static bool MoreEfficient(MachineId_t a, MachineId_t b) {
    return Efficiency(a) > Efficiency(b);
}

static unsigned StandbyTarget(CPUType_t cpu) {
    unsigned target = unsigned(type_count[cpu] * STANDBY_SHARE);
    if (target == 0 && type_count[cpu] > MIN_RUNNING) {
        target = 1;
    }
    return target;
}

static unsigned CountStandby(CPUType_t cpu) {
    unsigned count = 0;
    for (unsigned i = 0; i < active_machines; i++) {
        if ((tier[i] == STANDBY || tier[i] == GOING_STANDBY) &&
            Machine_GetCPUType(i) == cpu) {
            count++;
        }
    }
    return count;
}

static bool FindVM(MachineId_t machine_id, VMType_t vm_type,
                   vector<VMId_t> &vms, VMId_t &found) {
    for (auto &vm : vms) {
        VMInfo_t vm_info = VM_GetInfo(vm);
        if (vm_info.machine_id == machine_id && vm_info.vm_type == vm_type) {
            found = vm;
            return true;
        }
    }
    return false;
}

static void WakeMachine(CPUType_t cpu, bool want_gpu) {
    // Placement preferences:
    //   1. Prefer machines in standby over machines that are off
    //   2. Prefer machines that match the GPU requirement
    //   3. Among equally preferred machines, choose the most efficient
    MachineId_t best = 0;
    bool found = false;
    bool best_standby = false;
    bool best_gpu = false;

    for (unsigned i = 0; i < active_machines; i++) {
        // Only use sleeping machines with the right CPU
        if ((tier[i] != STANDBY && tier[i] != OFF) ||
            Machine_GetCPUType(i) != cpu) {
            continue;
        }

        bool is_standby = (tier[i] == STANDBY);
        bool gpu_match = (Machine_GetInfo(i).gpus == want_gpu);

        if (!found ||
            (is_standby != best_standby && is_standby) ||
            (is_standby == best_standby && gpu_match != best_gpu && gpu_match) ||
            (is_standby == best_standby && gpu_match == best_gpu &&
             Efficiency(i) > Efficiency(best))) {

            best = i;
            best_standby = is_standby;
            best_gpu = gpu_match;
            found = true;
        }
    }

    if (found) {
        tier[best] = WAKING;
        Machine_SetState(best, S0);
    }
}

static void CheckLoad(CPUType_t cpu, bool want_gpu) {
    // Add up the load and capacity of running and waking machines
    unsigned load = 0;
    unsigned capacity = 0;

    for (unsigned i = 0; i < active_machines; i++) {
        if ((tier[i] != RUNNING && tier[i] != WAKING) ||
            Machine_GetCPUType(i) != cpu) {
            continue;
        }
        MachineInfo_t machine = Machine_GetInfo(i);
        capacity += machine.num_cpus;
        if (tier[i] == RUNNING) {
            load += machine.active_tasks;
        }
    }

    // Tasks waiting for a machine also count as load
    for (auto &task_id : pending) {
        if (RequiredCPUType(task_id) == cpu) {
            load++;
        }
    }

    // Wake up another machine if the load is too high
    if (load > 0 && (capacity == 0 || load > UP_THRESHOLD * capacity)) {
        WakeMachine(cpu, want_gpu);
    }
}

static void SleepMachine(MachineId_t machine_id, vector<VMId_t> &vms,
                         MachineState_t state) {
    // Shut down the empty VMs on this machine
    VMId_t vm;
    for (unsigned t = LINUX; t <= AIX; t++) {
        while (FindVM(machine_id, VMType_t(t), vms, vm)) {
            VM_Shutdown(vm);
            vms.erase(find(vms.begin(), vms.end(), vm));
        }
    }

    tier[machine_id] = (state == STANDBY_STATE) ? GOING_STANDBY : GOING_OFF;
    Machine_SetState(machine_id, state);
}

static bool PlaceTask(TaskId_t task_id, vector<VMId_t> &vms) {
    // Get task requirements
    CPUType_t required_cpu = RequiredCPUType(task_id);
    VMType_t required_vm = RequiredVMType(task_id);
    unsigned required_memory = GetTaskMemory(task_id);
    bool needs_gpu = IsTaskGPUCapable(task_id);

    // Assign scheduling priority based on SLA
    Priority_t priority = LOW_PRIORITY;
    switch (RequiredSLA(task_id)) {
        case SLA0:
            priority = HIGH_PRIORITY;
            break;
        case SLA1:
        case SLA2:
            priority = MID_PRIORITY;
            break;
        case SLA3:
            priority = LOW_PRIORITY;
            break;
    }

    // Placement preferences:
    //   1. Prefer machines that have a free core
    //   2. Among those, prefer machines that match the GPU requirement
    //   3. Then choose the machine with the most tasks
    //   4. If every machine is full, choose the least loaded one
    MachineId_t best = 0;
    bool found = false;
    bool best_open = false;
    bool best_gpu = false;
    double best_load = 0;

    for (unsigned i = 0; i < active_machines; i++) {
        // Only use running machines with the right CPU
        if (tier[i] != RUNNING || Machine_GetCPUType(i) != required_cpu) {
            continue;
        }
        MachineInfo_t machine = Machine_GetInfo(i);

        // Check memory requirement
        VMId_t vm;
        unsigned needed_memory = required_memory;
        if (!FindVM(i, required_vm, vms, vm)) {
            needed_memory += VM_MEMORY_OVERHEAD;
        }
        if (machine.memory_used + needed_memory > machine.memory_size) {
            continue;
        }

        bool open = machine.active_tasks < machine.num_cpus;
        bool gpu_match = (machine.gpus == needs_gpu);
        double load = double(machine.active_tasks) / machine.num_cpus;

        bool better;
        if (!found) {
            better = true;
        } else if (open != best_open) {
            better = open;
        } else if (!open) {
            better = load < best_load;
        } else if (gpu_match != best_gpu) {
            better = gpu_match;
        } else {
            better = load > best_load;
        }

        if (better) {
            best = i;
            best_open = open;
            best_gpu = gpu_match;
            best_load = load;
            found = true;
        }
    }

    // No suitable machine was found
    if (!found) {
        return false;
    }

    // Reuse a compatible VM on the machine or create a new one
    VMId_t target_vm;
    if (!FindVM(best, required_vm, vms, target_vm)) {
        target_vm = VM_Create(required_vm, required_cpu);
        VM_Attach(target_vm, best);
        vms.push_back(target_vm);
    }

    VM_AddTask(target_vm, task_id, priority);
    SimOutput("Scheduler::PlaceTask(): Task " +
              to_string(task_id) +
              " assigned to machine " +
              to_string(best), 3);

    // Add capacity if the machine was already full
    if (!best_open) {
        CheckLoad(required_cpu, needs_gpu);
    }
    return true;
}

void Scheduler::Init() {
    active_machines = Machine_GetTotal();
    tier.assign(active_machines, RUNNING);
    last_busy.assign(active_machines, 0);

    for (unsigned i = 0; i < active_machines; i++) {
        machines.push_back(MachineId_t(i));
        type_count[Machine_GetCPUType(i)]++;
    }

    // For each CPU type keep the most efficient machines running,
    // put the next ones in standby and turn off the rest
    for (unsigned cpu = ARM; cpu <= X86; cpu++) {
        vector<MachineId_t> group;
        for (auto &machine_id : machines) {
            if (Machine_GetCPUType(machine_id) == CPUType_t(cpu)) {
                group.push_back(machine_id);
            }
        }
        sort(group.begin(), group.end(), MoreEfficient);

        unsigned standby = StandbyTarget(CPUType_t(cpu));
        for (unsigned j = MIN_RUNNING; j < group.size(); j++) {
            if (j < MIN_RUNNING + standby) {
                SleepMachine(group[j], vms, STANDBY_STATE);
            } else {
                SleepMachine(group[j], vms, OFF_STATE);
            }
        }
    }
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    // Hold the task until a machine wakes up
    if (!PlaceTask(task_id, vms)) {
        pending.push_back(task_id);
        CheckLoad(RequiredCPUType(task_id), IsTaskGPUCapable(task_id));
    }
}

void Scheduler::PeriodicCheck(Time_t now) {
    if (now - last_check < CHECK_INTERVAL) {
        return;
    }
    last_check = now;

    // Try to place tasks that are waiting for a machine
    vector<TaskId_t> waiting;
    waiting.swap(pending);
    for (auto &task_id : waiting) {
        if (!PlaceTask(task_id, vms)) {
            pending.push_back(task_id);
        }
    }

    for (unsigned cpu = ARM; cpu <= X86; cpu++) {
        CheckLoad(CPUType_t(cpu), false);

        // Add up the load and capacity of the running machines
        unsigned load = 0;
        unsigned capacity = 0;
        unsigned running = 0;
        for (auto &machine_id : machines) {
            if (tier[machine_id] != RUNNING ||
                Machine_GetCPUType(machine_id) != CPUType_t(cpu)) {
                continue;
            }
            MachineInfo_t machine = Machine_GetInfo(machine_id);
            running++;
            load += machine.active_tasks;
            capacity += machine.num_cpus;
            if (machine.active_tasks > 0) {
                last_busy[machine_id] = now;
            }
        }

        // Don't shrink while tasks are waiting for this CPU type
        bool has_pending = false;
        for (auto &task_id : pending) {
            if (RequiredCPUType(task_id) == CPUType_t(cpu)) {
                has_pending = true;
            }
        }

        // Put idle machines to sleep while the load is low
        for (auto &machine_id : machines) {
            if (has_pending || running <= MIN_RUNNING) {
                break;
            }
            if (tier[machine_id] != RUNNING ||
                Machine_GetCPUType(machine_id) != CPUType_t(cpu) ||
                now - last_busy[machine_id] < IDLE_WAIT) {
                continue;
            }
            MachineInfo_t machine = Machine_GetInfo(machine_id);
            if (machine.active_tasks > 0) {
                continue;
            }

            // Load must stay low without this machine
            unsigned remaining = capacity - machine.num_cpus;
            if (load >= DOWN_THRESHOLD * remaining) {
                continue;
            }

            // Fill the standby tier first, otherwise turn it off
            if (CountStandby(CPUType_t(cpu)) < StandbyTarget(CPUType_t(cpu))) {
                SleepMachine(machine_id, vms, STANDBY_STATE);
            } else {
                SleepMachine(machine_id, vms, OFF_STATE);
            }
            running--;
            capacity = remaining;
        }

        // Refill the standby tier from machines that are off
        if (CountStandby(CPUType_t(cpu)) < StandbyTarget(CPUType_t(cpu))) {
            for (auto &machine_id : machines) {
                if (tier[machine_id] == OFF &&
                    Machine_GetCPUType(machine_id) == CPUType_t(cpu)) {
                    tier[machine_id] = GOING_STANDBY;
                    Machine_SetState(machine_id, STANDBY_STATE);
                    break;
                }
            }
        }
    }
}

void Scheduler::Shutdown(Time_t time) {
    for (auto &vm : vms) {
        VM_Shutdown(vm);
    }
    SimOutput("SimulationComplete(): Finished!", 4);
    SimOutput("SimulationComplete(): Time is " +
              to_string(time), 4);
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    SimOutput("Scheduler::TaskComplete(): Task " +
              to_string(task_id) +
              " completed at time " +
              to_string(now), 4);
}

// Public interface

static Scheduler Scheduler;

void InitScheduler() {
    SimOutput("InitScheduler(): Initializing scheduler", 4);
    Scheduler.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    SimOutput("HandleNewTask(): Received new task " +
              to_string(task_id) +
              " at time " + to_string(time), 4);
    Scheduler.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    SimOutput("HandleTaskCompletion(): Task " +
              to_string(task_id) +
              " completed at time " +
              to_string(time), 4);
    Scheduler.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    SimOutput("MemoryWarning(): Overflow at machine " +
              to_string(machine_id) +
              " at time " + to_string(time), 0);
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    SimOutput("MigrationDone(): Migration of VM " +
              to_string(vm_id) + " completed", 4);
    Scheduler.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    SimOutput("SchedulerCheck(): Called at time " +
              to_string(time), 4);
    Scheduler.PeriodicCheck(time);
}

void SimulationComplete(Time_t time) {
    cout << "SLA violation report" << endl;
    cout << "SLA0: " << GetSLAReport(SLA0) << "%" << endl;
    cout << "SLA1: " << GetSLAReport(SLA1) << "%" << endl;
    cout << "SLA2: " << GetSLAReport(SLA2) << "%" << endl;
    cout << "Total Energy "
         << Machine_GetClusterEnergy()
         << "KW-Hour" << endl;
    cout << "Simulation run finished in "
         << double(time) / 1000000
         << " seconds" << endl;
    Scheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    CheckLoad(RequiredCPUType(task_id), IsTaskGPUCapable(task_id));
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    // Update the tier based on the state the machine reached
    MachineState_t state = Machine_GetInfo(machine_id).s_state;
    if (state == S0) {
        tier[machine_id] = RUNNING;
        last_busy[machine_id] = time;
    } else if (state == STANDBY_STATE) {
        tier[machine_id] = STANDBY;
    } else if (state == OFF_STATE) {
        tier[machine_id] = OFF;
    }
}