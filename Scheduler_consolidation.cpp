
#include <cstdint>
#include <limits>
#include "Scheduler.hpp"

static unsigned active_machines = 0;

void Scheduler::Init() {
    active_machines = Machine_GetTotal();
    for (unsigned i = 0; i < active_machines; i++) {
        machines.push_back(MachineId_t(i));
    }
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
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
            priority = MID_PRIORITY;
            break;
        case SLA2:
            priority = MID_PRIORITY;
            break;
        case SLA3:
            priority = LOW_PRIORITY;
            break;
    }

    // First select the compatible VM whose host machine
    // will have the least remaining memory after task placement

    VMId_t best_vm = 0;
    unsigned best_vm_remaining =
        std::numeric_limits<unsigned>::max();
    bool found_vm = false;

    for (auto &vm : vms) {
        VMInfo_t vm_info = VM_GetInfo(vm);

        // Check CPU requirement
        if (vm_info.cpu != required_cpu ||
            vm_info.vm_type != required_vm) {
            continue;
        }

        MachineInfo_t machine =
            Machine_GetInfo(vm_info.machine_id);

        // Only use machines that are currently awake
        if (machine.s_state != S0) {
            continue;
        }

        // Check GPU requirement
        if (needs_gpu && !machine.gpus) {
            continue;
        }

        // Check task memory requirement
        if (machine.memory_used > machine.memory_size) {
            continue;
        }

        unsigned available_memory =
            machine.memory_size - machine.memory_used;
        if (required_memory > available_memory) {
            continue;
        }

        unsigned remaining_memory =
            available_memory - required_memory;

        // Choose the VM with the least remaining memory on its host machine
        if (!found_vm || remaining_memory < best_vm_remaining) {
            best_vm = vm;
            best_vm_remaining = remaining_memory;
            found_vm = true;
        }
    }

    // Assign the task to the best existing VM
    if (found_vm) {
        VM_AddTask(best_vm, task_id, priority);
        SimOutput("Scheduler::NewTask(): Task " +
            to_string(task_id) +
            " assigned to existing VM " +
            to_string(best_vm), 3);
        return;
    }

    // Next try to find the best physical machine for a new VM
    //
    // Placement preferences:
    //   1. Prefer machines already hosting VMs
    //   2. Among equally preferred machines, choose the one
    //      with the least remaining memory

    MachineId_t best_machine = 0;
    unsigned best_remaining_memory = std::numeric_limits<unsigned>::max();
    bool best_machine_is_active = false;
    bool found_machine = false;
    unsigned needed_memory = required_memory + VM_MEMORY_OVERHEAD;

    for (auto &machine_id : machines) {
        MachineInfo_t machine =
            Machine_GetInfo(machine_id);

        // Check CPU requirement
        if (machine.cpu != required_cpu) {
            continue;
        }

        // Only use machines that are currently awake
        if (machine.s_state != S0) {
            continue;
        }

        // Check GPU requirement
        if (needs_gpu && !machine.gpus) {
            continue;
        }

        // Check memory requirement
        if (machine.memory_used > machine.memory_size) {
            continue;
        }

        unsigned available_memory =
            machine.memory_size - machine.memory_used;
        if (needed_memory > available_memory) {
            continue;
        }

        unsigned remaining_memory =
            available_memory - needed_memory;

        // Determine whether the machine already hosts VMs
        bool machine_is_active = (machine.active_vms > 0);

        // Prefer machines already hosting VMs
        // If both machines have the same preference,
        // choose the one with the least remaining memory
        if (!found_machine ||
            (machine_is_active && !best_machine_is_active) ||
            (machine_is_active == best_machine_is_active &&
             remaining_memory < best_remaining_memory)) {

            best_machine = machine_id;
            best_remaining_memory = remaining_memory;
            best_machine_is_active = machine_is_active;
            found_machine = true;
        }
    }

    // Create a VM on the selected physical machine
    if (found_machine) {
        VMId_t new_vm = VM_Create(required_vm, required_cpu);
        VM_Attach(new_vm, best_machine);
        vms.push_back(new_vm);
        VM_AddTask(new_vm, task_id, priority);
        SimOutput("Scheduler::NewTask(): Created VM " +
                  to_string(new_vm) +
                  " on machine " +
                  to_string(best_machine) +
                  " for task " +
                  to_string(task_id), 2);
        return;
    }

    // No suitable machine was found
    SimOutput("Scheduler::NewTask(): WARNING - No compatible "
              "machine available for task " +
              to_string(task_id), 0);
}

void Scheduler::PeriodicCheck(Time_t now) {
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
}
