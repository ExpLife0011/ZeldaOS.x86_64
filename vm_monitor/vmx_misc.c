/*
 * Copyright (c) 2019 Jie Zheng
 */

#include <x86_64/include/msr.h>
#include <x86_64/include/cpuid.h>
#include <lib64/include/logging.h>
#include <x86_64/include/lapic.h>
#include <memory/include/paging.h>
#include <lib64/include/string.h>

#define VMXON_LOCK_FLAG 0x1
#define VMXON_ENABLE_FLAG 0x4
#define VMX_ENABLE_FLAG (1 << 13)

int
is_vmx_support(void)
{
    uint32_t eax = 1, ebx, ecx, edx;
    CPUID(eax, ebx, ecx, edx);
    return !!(ecx & CPUID_FEAT_ECX_VMX);
}

void
check_vmx_feature(void)
{
    int supported = is_vmx_support();
    LOG_INFO("cpu feature:vmx %s\n", supported ? "supported" : "not supported");
}

uint32_t
get_vmx_revision_id(void)
{
    uint32_t ecx = IA32_VMX_BASIC_MSR;
    uint32_t eax;
    uint32_t edx;
    RDMSR(ecx, &eax, &edx);
    ASSERT(!(eax & 0x80000000));
    return eax;
}

void
vm_monitor_init(void)
{
    int this_cpu = cpu();
    check_vmx_feature();
    if (!is_vmx_support()) {
        LOG_WARN("vmx feature not supported\n");
        return;
    }
    {
        // Make sure VMX is enabled in IA32_FEATURE_CONTROL_MSR
        uint32_t ecx = IA32_FEATURE_CONTROL_MSR;
        uint32_t eax;
        uint32_t edx;
        RDMSR(ecx, &eax, &edx);

        // If BIOS lock the vmx, we should not go on. Go to configure BIOS to
        // enable intel VT-x...
        ASSERT(!(eax & VMXON_LOCK_FLAG));

        if (!(eax & VMXON_ENABLE_FLAG)) {
            ecx = IA32_FEATURE_CONTROL_MSR;
            eax |= VMXON_ENABLE_FLAG;
            WRMSR(ecx, eax, edx);
        }
        ecx = IA32_FEATURE_CONTROL_MSR;
        RDMSR(ecx, &eax, &edx);
        ASSERT(eax & VMXON_ENABLE_FLAG);
        LOG_INFO("cpu:%d Enable vmxon flag in IA32_FEATURE_CONTROL_MSR\n",
                  this_cpu);
    }
    {
        // Enable vmx in CR4
        uint64_t cr4;
        __asm__ volatile("movq %%cr4, %%rax;"
                         "orq %%rdx, %%rax;"
                         "movq %%rax, %%cr4"
                         :"=a"(cr4)
                         :"d"(VMX_ENABLE_FLAG));
        LOG_INFO("cpu:%d enable vmx enable flag in CR4 register\n", this_cpu);
    }
    {
        // https://www.felixcloutier.com/x86/vmxon
        uint32_t revision_id = get_vmx_revision_id();
        uint64_t vmxon_region = get_physical_page();
        uint64_t vmxon_region_pa = pa(vmxon_region);
        ASSERT(vmxon_region);
        memset((void *)vmxon_region, 0x0, PAGE_SIZE_4K);
        LOG_INFO("cpu:%d vmx region:0x%x(pa:0x%x)\n",
                 this_cpu, vmxon_region, vmxon_region_pa);
        LOG_INFO("cpu:%d vmx revision id:0x%x\n", this_cpu, revision_id); 
        *(uint32_t *)vmxon_region = revision_id;

        uint16_t successfull = 0;
        __asm__ volatile("xorw %%dx, %%dx;"
                         "movw $0x1, %%cx;"
                         "vmxon (%%rax);"
                         "cmovnc %%cx, %%dx;"
                         :"=d"(successfull)
                         :"a"(vmxon_region_pa)
                         :"memory", "cc", "%rcx");
        ASSERT(successfull);
        LOG_INFO("enter vmx root mode: %s\n",
                 successfull ? "successfull" : "unsuccessfull");
        // Now the CPU is in vmx root operation, the INIT# and A20M# are blocked
        // the CR4.vmxe can be cleared only the cpu execute VMXOFF instruction
    }
}
