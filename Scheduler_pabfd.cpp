
#include <cstdint>
#include "Scheduler.hpp"

#define SLEEP_STATE     S1
#define IDLE_WAIT       1000000
#define CHECK_INTERVAL  100000

static unsigned active_machines = 0;
static vector<bool> changing;
static vector<Time_t> last_busy;
static vector<TaskId_t> pending;
static Time_t last_check = 0;

// Power model: P(u) = idle + (peak - idle) * u
// Placing one more task adds the power of one busy core
static double PowerIncrease(MachineInfo_t &machine) {
    return machine.p_states[machine.p_state] - machine.c_states[C1];
}

static unsigned CountPending(CPUType_t cpu) {
    unsigned count = 0;
    for (auto &task_id : pending) {
        if (RequiredCPUType(task_id) == cpu) {
            count++;
        }
    }
    return count;
}

static bool PlaceTask(TaskId_t task_id, vector<VMId_t> &vms) {
    // Get task requirements
    CPUType_t required_cpu = RequiredCPUType(task_id);
    VMType_t required_vm = RequiredVMType(task_id);
    unsigned required_memory = GetTaskMemory(task_id);

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

    // Choose the machine whose power increases the least
    // If two machines tie, choose the one with more tasks
    MachineId_t best_machine = 0;
    double best_increase = 0;
    unsigned best_tasks = 0;
    bool found_machine = false;

    for (unsigned i = 0; i < active_machines; i++) {
        MachineInfo_t machine = Machine_GetInfo(i);

        // Only use machines that are awake
        if (machine.s_state != S0 || changing[i]) {
            continue;
        }

        // Check CPU requirement
        if (machine.cpu != required_cpu) {
            continue;
        }

        // Do not overload the machine
        if (machine.active_tasks >= machine.num_cpus) {
            continue;
        }

        // Check memory requirement
        if (machine.memory_used + required_memory + VM_MEMORY_OVERHEAD >
            machine.memory_size) {
            continue;
        }

        double increase = PowerIncrease(machine);
        if (!found_machine || increase < best_increase ||
            (increase == best_increase &&
             machine.active_tasks > best_tasks)) {
            best_machine = i;
            best_increase = increase;
            best_tasks = machine.active_tasks;
            found_machine = true;
        }
    }

    if (!found_machine) {
        return false;
    }

    // Reuse a VM on the machine or create a new one
    for (auto &vm : vms) {
        VMInfo_t vm_info = VM_GetInfo(vm);
        if (vm_info.machine_id == best_machine &&
            vm_info.vm_type == required_vm) {
            VM_AddTask(vm, task_id, priority);
            return true;
        }
    }

    VMId_t new_vm = VM_Create(required_vm, required_cpu);
    VM_Attach(new_vm, best_machine);
    vms.push_back(new_vm);
    VM_AddTask(new_vm, task_id, priority);
    return true;
}

static void WakeMachine(CPUType_t cpu) {
    // Count the cores that are already waking up
    unsigned waking_cores = 0;
    for (unsigned i = 0; i < active_machines; i++) {
        MachineInfo_t machine = Machine_GetInfo(i);
        if (changing[i] && machine.s_state != S0 && machine.cpu == cpu) {
            waking_cores += machine.num_cpus;
        }
    }
    if (CountPending(cpu) <= waking_cores) {
        return;
    }

    // Wake the sleeping machine that uses the least power
    MachineId_t best_machine = 0;
    double best_power = 0;
    bool found_machine = false;

    for (unsigned i = 0; i < active_machines; i++) {
        MachineInfo_t machine = Machine_GetInfo(i);
        if (machine.s_state == S0 || changing[i] || machine.cpu != cpu) {
            continue;
        }

        double power = machine.s_states[S0] +
                       machine.num_cpus * machine.c_states[C1] +
                       PowerIncrease(machine);
        if (!found_machine || power < best_power) {
            best_machine = i;
            best_power = power;
            found_machine = true;
        }
    }

    if (found_machine) {
        changing[best_machine] = true;
        Machine_SetState(best_machine, S0);
    }
}

void Scheduler::Init() {
    active_machines = Machine_GetTotal();
    changing.assign(active_machines, false);
    last_busy.assign(active_machines, 0);
    for (unsigned i = 0; i < active_machines; i++) {
        machines.push_back(MachineId_t(i));
    }
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    // If no machine fits, hold the task and wake a machine
    if (!PlaceTask(task_id, vms)) {
        pending.push_back(task_id);
        WakeMachine(RequiredCPUType(task_id));
    }
}

void Scheduler::PeriodicCheck(Time_t now) {
    if (now - last_check < CHECK_INTERVAL) {
        return;
    }
    last_check = now;

    // Try to place tasks that are waiting
    vector<TaskId_t> waiting;
    waiting.swap(pending);
    for (auto &task_id : waiting) {
        if (!PlaceTask(task_id, vms)) {
            pending.push_back(task_id);
            WakeMachine(RequiredCPUType(task_id));
        }
    }

    // Put machines to sleep once they have been idle for a while
    for (auto &machine_id : machines) {
        MachineInfo_t machine = Machine_GetInfo(machine_id);
        if (machine.s_state != S0 || changing[machine_id]) {
            continue;
        }
        if (machine.active_tasks > 0) {
            last_busy[machine_id] = now;
            continue;
        }
        if (now - last_busy[machine_id] < IDLE_WAIT ||
            CountPending(machine.cpu) > 0) {
            continue;
        }

        // Shut down the empty VMs on the machine first
        for (unsigned i = 0; i < vms.size(); ) {
            if (VM_GetInfo(vms[i]).machine_id == machine_id) {
                VM_Shutdown(vms[i]);
                vms.erase(vms.begin() + i);
            } else {
                i++;
            }
        }

        changing[machine_id] = true;
        Machine_SetState(machine_id, SLEEP_STATE);
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
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    changing[machine_id] = false;
    last_busy[machine_id] = time;
}
