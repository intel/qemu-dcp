/*
 * QEMU emulation of an Intel IOMMU (VT-d)
 *   (DMA Remapping device)
 *
 * Copyright (C) 2013 Knut Omang, Oracle <knut.omang@oracle.com>
 * Copyright (C) 2014 Le Tan, <tamlokveer@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "intel_iommu_internal.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/qdev-properties.h"
#include "hw/i386/pc.h"
#include "hw/i386/apic-msidef.h"
#include "hw/i386/x86-iommu.h"
#include "hw/pci-host/q35.h"
#include "sysemu/kvm.h"
#include "sysemu/dma.h"
#include "sysemu/sysemu.h"
#include "hw/i386/apic_internal.h"
#include "kvm/kvm_i386.h"
#include "migration/vmstate.h"
#include "trace.h"
#include <linux/ioasid.h>
#include <sys/ioctl.h>
#include "qemu/jhash.h"
#include <linux/iommu.h>

#include "migration/migration.h"
#include "migration/qemu-file.h"
#include "migration/register.h"
#include "migration/blocker.h"
#include "migration/misc.h"

// Open special debug log by uncomment below line
//#define _VTD_DEBUG 1

#ifdef CONFIG_VTD_DEBUG
#define VTD_DEBUG(fmt, ...) do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define VTD_DEBUG(fmt, ...) do { } while (0)
#endif

int ioasid_fd = -1;
uint32_t ioasid_bits;

/* context entry operations */
#define VTD_CE_GET_RID2PASID(ce) \
    ((ce)->val[1] & VTD_SM_CONTEXT_ENTRY_RID2PASID_MASK)
#define VTD_CE_GET_PASID_DIR_TABLE(ce) \
    ((ce)->val[0] & VTD_PASID_DIR_BASE_ADDR_MASK)

/* pe operations */
#define VTD_PE_GET_TYPE(pe) ((pe)->val[0] & VTD_SM_PASID_ENTRY_PGTT)
#define VTD_PE_GET_LEVEL(pe) (2 + (((pe)->val[0] >> 2) & VTD_SM_PASID_ENTRY_AW))
#define VTD_PE_GET_FPD_ERR(ret_fr, is_fpd_set, s, source_id, addr, is_write) {\
    if (ret_fr) {                                                             \
        ret_fr = -ret_fr;                                                     \
        if (is_fpd_set && vtd_is_qualified_fault(ret_fr)) {                   \
            trace_vtd_fault_disabled();                                       \
        } else {                                                              \
            vtd_report_dmar_fault(s, source_id, addr, ret_fr, is_write);      \
        }                                                                     \
        goto error;                                                           \
    }                                                                         \
}

#define QI_RESP_INVALID         0x1

static void vtd_address_space_refresh_all(IntelIOMMUState *s);
static void vtd_address_space_unmap(VTDAddressSpace *as, IOMMUNotifier *n);

static void vtd_pasid_cache_reset(IntelIOMMUState *s);
static void vtd_pasid_cache_sync(IntelIOMMUState *s,
                                 VTDPASIDCacheInfo *pc_info);
static void vtd_pasid_cache_devsi(IntelIOMMUState *s,
                                  VTDBus *vtd_bus, uint16_t devfn);
static VTDPASIDAddressSpace *vtd_add_find_pasid_as(IntelIOMMUState *s,
                                                   VTDBus *vtd_bus,
                                                   int devfn,
                                                   uint32_t pasid);
static VTDBus *vtd_find_add_bus(IntelIOMMUState *s, PCIBus *bus);
static int vtd_dev_get_rid2pasid(IntelIOMMUState *s,
                                 uint8_t bus_num, uint8_t devfn);
static gboolean vtd_hash_remove_by_pasid(gpointer key, gpointer value,
                                         gpointer user_data);
static int vtd_dev_send_page_response(IntelIOMMUState *s, PCIBus *bus,
                                      int devfn,
                                      struct iommu_page_response *pg_resp);
static void vtd_assemble_pg_resp(struct iommu_page_response *pg_resp,
                                 VTDPageReqDsc prq, int code);

static void vtd_panic_require_caching_mode(void)
{
    error_report("We need to set caching-mode=on for intel-iommu to enable "
                 "device assignment with IOMMU protection.");
    exit(1);
}

static void vtd_define_quad(IntelIOMMUState *s, hwaddr addr, uint64_t val,
                            uint64_t wmask, uint64_t w1cmask)
{
    stq_le_p(&s->csr[addr], val);
    stq_le_p(&s->wmask[addr], wmask);
    stq_le_p(&s->w1cmask[addr], w1cmask);
}

static void vtd_define_quad_wo(IntelIOMMUState *s, hwaddr addr, uint64_t mask)
{
    stq_le_p(&s->womask[addr], mask);
}

static void vtd_define_long(IntelIOMMUState *s, hwaddr addr, uint32_t val,
                            uint32_t wmask, uint32_t w1cmask)
{
    stl_le_p(&s->csr[addr], val);
    stl_le_p(&s->wmask[addr], wmask);
    stl_le_p(&s->w1cmask[addr], w1cmask);
}

static void vtd_define_long_wo(IntelIOMMUState *s, hwaddr addr, uint32_t mask)
{
    stl_le_p(&s->womask[addr], mask);
}

/* "External" get/set operations */
static void vtd_set_quad(IntelIOMMUState *s, hwaddr addr, uint64_t val)
{
    uint64_t oldval = ldq_le_p(&s->csr[addr]);
    uint64_t wmask = ldq_le_p(&s->wmask[addr]);
    uint64_t w1cmask = ldq_le_p(&s->w1cmask[addr]);
    stq_le_p(&s->csr[addr],
             ((oldval & ~wmask) | (val & wmask)) & ~(w1cmask & val));
}

static void vtd_set_long(IntelIOMMUState *s, hwaddr addr, uint32_t val)
{
    uint32_t oldval = ldl_le_p(&s->csr[addr]);
    uint32_t wmask = ldl_le_p(&s->wmask[addr]);
    uint32_t w1cmask = ldl_le_p(&s->w1cmask[addr]);
    stl_le_p(&s->csr[addr],
             ((oldval & ~wmask) | (val & wmask)) & ~(w1cmask & val));
}

static uint64_t vtd_get_quad(IntelIOMMUState *s, hwaddr addr)
{
    uint64_t val = ldq_le_p(&s->csr[addr]);
    uint64_t womask = ldq_le_p(&s->womask[addr]);
    return val & ~womask;
}

static uint32_t vtd_get_long(IntelIOMMUState *s, hwaddr addr)
{
    uint32_t val = ldl_le_p(&s->csr[addr]);
    uint32_t womask = ldl_le_p(&s->womask[addr]);
    return val & ~womask;
}

/* "Internal" get/set operations */
static uint64_t vtd_get_quad_raw(IntelIOMMUState *s, hwaddr addr)
{
    return ldq_le_p(&s->csr[addr]);
}

static uint32_t vtd_get_long_raw(IntelIOMMUState *s, hwaddr addr)
{
    return ldl_le_p(&s->csr[addr]);
}

static void vtd_set_quad_raw(IntelIOMMUState *s, hwaddr addr, uint64_t val)
{
    stq_le_p(&s->csr[addr], val);
}

static uint32_t vtd_set_clear_mask_long(IntelIOMMUState *s, hwaddr addr,
                                        uint32_t clear, uint32_t mask)
{
    uint32_t new_val = (ldl_le_p(&s->csr[addr]) & ~clear) | mask;
    stl_le_p(&s->csr[addr], new_val);
    return new_val;
}

static uint64_t vtd_set_clear_mask_quad(IntelIOMMUState *s, hwaddr addr,
                                        uint64_t clear, uint64_t mask)
{
    uint64_t new_val = (ldq_le_p(&s->csr[addr]) & ~clear) | mask;
    stq_le_p(&s->csr[addr], new_val);
    return new_val;
}

static inline void vtd_iommu_lock(IntelIOMMUState *s)
{
    qemu_mutex_lock(&s->iommu_lock);
}

static inline void vtd_iommu_unlock(IntelIOMMUState *s)
{
    qemu_mutex_unlock(&s->iommu_lock);
}

static void vtd_update_scalable_state(IntelIOMMUState *s)
{
    uint64_t val = vtd_get_quad_raw(s, DMAR_RTADDR_REG);

    if (s->scalable_mode) {
        s->root_scalable = val & VTD_RTADDR_SMT;
    }
}

/* Whether the address space needs to notify new mappings */
static inline gboolean vtd_as_has_map_notifier(VTDAddressSpace *as)
{
    return as->notifier_flags & IOMMU_NOTIFIER_MAP;
}

/* GHashTable functions */
static gboolean vtd_uint64_equal(gconstpointer v1, gconstpointer v2)
{
    return *((const uint64_t *)v1) == *((const uint64_t *)v2);
}

static guint vtd_uint64_hash(gconstpointer v)
{
    return (guint)*(const uint64_t *)v;
}

static gboolean vtd_hash_remove_by_domain(gpointer key, gpointer value,
                                          gpointer user_data)
{
    VTDIOTLBEntry *entry = (VTDIOTLBEntry *)value;
    uint16_t domain_id = *(uint16_t *)user_data;
    return entry->domain_id == domain_id;
}

/* The shift of an addr for a certain level of paging structure */
static inline uint32_t vtd_slpt_level_shift(uint32_t level)
{
    assert(level != 0);
    return VTD_PAGE_SHIFT_4K + (level - 1) * VTD_SL_LEVEL_BITS;
}

static inline uint64_t vtd_slpt_level_page_mask(uint32_t level)
{
    return ~((1ULL << vtd_slpt_level_shift(level)) - 1);
}

static gboolean vtd_hash_remove_by_page(gpointer key, gpointer value,
                                        gpointer user_data)
{
    VTDIOTLBEntry *entry = (VTDIOTLBEntry *)value;
    VTDIOTLBPageInvInfo *info = (VTDIOTLBPageInvInfo *)user_data;
    uint64_t gfn = (info->addr >> VTD_PAGE_SHIFT_4K) & info->mask;
    uint64_t gfn_tlb = (info->addr & entry->mask) >> VTD_PAGE_SHIFT_4K;
    return (entry->domain_id == info->domain_id) &&
            (info->is_piotlb ? (entry->pasid == info->pasid) : 1) &&
            (((entry->gfn & info->mask) == gfn) ||
             (entry->gfn == gfn_tlb));
}

/* Reset all the gen of VTDAddressSpace to zero and set the gen of
 * IntelIOMMUState to 1.  Must be called with IOMMU lock held.
 */
static void vtd_reset_context_cache_locked(IntelIOMMUState *s)
{
    VTDAddressSpace *vtd_as;
    VTDBus *vtd_bus;
    GHashTableIter bus_it;
    uint32_t devfn_it;

    trace_vtd_context_cache_reset();

    g_hash_table_iter_init(&bus_it, s->vtd_as_by_busptr);

    while (g_hash_table_iter_next (&bus_it, NULL, (void**)&vtd_bus)) {
        for (devfn_it = 0; devfn_it < PCI_DEVFN_MAX; ++devfn_it) {
            vtd_as = vtd_bus->dev_as[devfn_it];
            if (!vtd_as) {
                continue;
            }
            vtd_as->context_cache_entry.context_cache_gen = 0;
        }
    }
    s->context_cache_gen = 1;
}

/* Must be called with IOMMU lock held. */
static void vtd_reset_iotlb_locked(IntelIOMMUState *s)
{
    assert(s->iotlb);
    g_hash_table_remove_all(s->iotlb);
}

static void vtd_reset_iotlb(IntelIOMMUState *s)
{
    vtd_iommu_lock(s);
    vtd_reset_iotlb_locked(s);
    vtd_iommu_unlock(s);
}

static void vtd_reset_piotlb(IntelIOMMUState *s)
{
    assert(s->p_iotlb);
    g_hash_table_remove_all(s->p_iotlb);
}

static void vtd_reset_caches(IntelIOMMUState *s)
{
    vtd_iommu_lock(s);
    vtd_reset_iotlb_locked(s);
    vtd_reset_context_cache_locked(s);
    vtd_pasid_cache_reset(s);
    vtd_reset_piotlb(s);
    vtd_iommu_unlock(s);
}

static uint64_t vtd_get_iotlb_key(uint64_t gfn, uint16_t source_id,
                                  uint32_t level)
{
    return gfn | ((uint64_t)(source_id) << VTD_IOTLB_SID_SHIFT) |
           ((uint64_t)(level) << VTD_IOTLB_LVL_SHIFT);
}

static uint64_t vtd_get_iotlb_gfn(hwaddr addr, uint32_t level)
{
    return (addr & vtd_slpt_level_page_mask(level)) >> VTD_PAGE_SHIFT_4K;
}

/* Must be called with IOMMU lock held */
static VTDIOTLBEntry *vtd_lookup_iotlb(IntelIOMMUState *s, uint16_t source_id,
                                       hwaddr addr)
{
    VTDIOTLBEntry *entry;
    uint64_t key;
    int level;

    for (level = VTD_SL_PT_LEVEL; level < VTD_SL_PML4_LEVEL; level++) {
        key = vtd_get_iotlb_key(vtd_get_iotlb_gfn(addr, level),
                                source_id, level);
        entry = g_hash_table_lookup(s->iotlb, &key);
        if (entry) {
            goto out;
        }
    }

out:
    return entry;
}

/* Must be with IOMMU lock held */
static void vtd_update_iotlb(IntelIOMMUState *s, uint16_t source_id,
                             uint16_t domain_id, hwaddr addr, uint64_t slpte,
                             uint8_t access_flags, uint32_t level)
{
    VTDIOTLBEntry *entry = g_malloc(sizeof(*entry));
    uint64_t *key = g_malloc(sizeof(*key));
    uint64_t gfn = vtd_get_iotlb_gfn(addr, level);

    trace_vtd_iotlb_page_update(source_id, addr, slpte, domain_id);
    if (g_hash_table_size(s->iotlb) >= VTD_IOTLB_MAX_SIZE) {
        trace_vtd_iotlb_reset("iotlb exceeds size limit");
        vtd_reset_iotlb_locked(s);
    }

    entry->gfn = gfn;
    entry->domain_id = domain_id;
    entry->pte = slpte;
    entry->access_flags = access_flags;
    entry->mask = vtd_slpt_level_page_mask(level);
    *key = vtd_get_iotlb_key(gfn, source_id, level);
    g_hash_table_replace(s->iotlb, key, entry);
}

/* Given the reg addr of both the message data and address, generate an
 * interrupt via MSI.
 */
static void vtd_generate_interrupt(IntelIOMMUState *s, hwaddr mesg_addr_reg,
                                   hwaddr mesg_data_reg)
{
    MSIMessage msi;

    assert(mesg_data_reg < DMAR_REG_SIZE);
    assert(mesg_addr_reg < DMAR_REG_SIZE);

    msi.address = vtd_get_long_raw(s, mesg_addr_reg);
    msi.data = vtd_get_long_raw(s, mesg_data_reg);

    trace_vtd_irq_generate(msi.address, msi.data);

    apic_get_class()->send_msi(&msi);
}

/* Generate a fault event to software via MSI if conditions are met.
 * Notice that the value of FSTS_REG being passed to it should be the one
 * before any update.
 */
static void vtd_generate_fault_event(IntelIOMMUState *s, uint32_t pre_fsts)
{
    if (pre_fsts & VTD_FSTS_PPF || pre_fsts & VTD_FSTS_PFO ||
        pre_fsts & VTD_FSTS_IQE) {
        error_report_once("There are previous interrupt conditions "
                          "to be serviced by software, fault event "
                          "is not generated");
        return;
    }
    vtd_set_clear_mask_long(s, DMAR_FECTL_REG, 0, VTD_FECTL_IP);
    if (vtd_get_long_raw(s, DMAR_FECTL_REG) & VTD_FECTL_IM) {
        error_report_once("Interrupt Mask set, irq is not generated");
    } else {
        vtd_generate_interrupt(s, DMAR_FEADDR_REG, DMAR_FEDATA_REG);
        vtd_set_clear_mask_long(s, DMAR_FECTL_REG, VTD_FECTL_IP, 0);
    }
}

/* Check if the Fault (F) field of the Fault Recording Register referenced by
 * @index is Set.
 */
static bool vtd_is_frcd_set(IntelIOMMUState *s, uint16_t index)
{
    /* Each reg is 128-bit */
    hwaddr addr = DMAR_FRCD_REG_OFFSET + (((uint64_t)index) << 4);
    addr += 8; /* Access the high 64-bit half */

    assert(index < DMAR_FRCD_REG_NR);

    return vtd_get_quad_raw(s, addr) & VTD_FRCD_F;
}

/* Update the PPF field of Fault Status Register.
 * Should be called whenever change the F field of any fault recording
 * registers.
 */
static void vtd_update_fsts_ppf(IntelIOMMUState *s)
{
    uint32_t i;
    uint32_t ppf_mask = 0;

    for (i = 0; i < DMAR_FRCD_REG_NR; i++) {
        if (vtd_is_frcd_set(s, i)) {
            ppf_mask = VTD_FSTS_PPF;
            break;
        }
    }
    vtd_set_clear_mask_long(s, DMAR_FSTS_REG, VTD_FSTS_PPF, ppf_mask);
    trace_vtd_fsts_ppf(!!ppf_mask);
}

static void vtd_set_frcd_and_update_ppf(IntelIOMMUState *s, uint16_t index)
{
    /* Each reg is 128-bit */
    hwaddr addr = DMAR_FRCD_REG_OFFSET + (((uint64_t)index) << 4);
    addr += 8; /* Access the high 64-bit half */

    assert(index < DMAR_FRCD_REG_NR);

    vtd_set_clear_mask_quad(s, addr, 0, VTD_FRCD_F);
    vtd_update_fsts_ppf(s);
}

/* Must not update F field now, should be done later */
static void vtd_record_frcd(IntelIOMMUState *s, uint16_t index,
                            uint16_t source_id, hwaddr addr,
                            VTDFaultReason fault, bool is_write)
{
    uint64_t hi = 0, lo;
    hwaddr frcd_reg_addr = DMAR_FRCD_REG_OFFSET + (((uint64_t)index) << 4);

    assert(index < DMAR_FRCD_REG_NR);

    lo = VTD_FRCD_FI(addr);
    hi = VTD_FRCD_SID(source_id) | VTD_FRCD_FR(fault);
    if (!is_write) {
        hi |= VTD_FRCD_T;
    }
    vtd_set_quad_raw(s, frcd_reg_addr, lo);
    vtd_set_quad_raw(s, frcd_reg_addr + 8, hi);

    trace_vtd_frr_new(index, hi, lo);
}

/* Try to collapse multiple pending faults from the same requester */
static bool vtd_try_collapse_fault(IntelIOMMUState *s, uint16_t source_id)
{
    uint32_t i;
    uint64_t frcd_reg;
    hwaddr addr = DMAR_FRCD_REG_OFFSET + 8; /* The high 64-bit half */

    for (i = 0; i < DMAR_FRCD_REG_NR; i++) {
        frcd_reg = vtd_get_quad_raw(s, addr);
        if ((frcd_reg & VTD_FRCD_F) &&
            ((frcd_reg & VTD_FRCD_SID_MASK) == source_id)) {
            return true;
        }
        addr += 16; /* 128-bit for each */
    }
    return false;
}

/* Log and report an DMAR (address translation) fault to software */
static void vtd_report_dmar_fault(IntelIOMMUState *s, uint16_t source_id,
                                  hwaddr addr, VTDFaultReason fault,
                                  bool is_write)
{
    uint32_t fsts_reg = vtd_get_long_raw(s, DMAR_FSTS_REG);

    assert(fault < VTD_FR_MAX);

    if (fault == VTD_FR_RESERVED_ERR) {
        /* This is not a normal fault reason case. Drop it. */
        return;
    }

    trace_vtd_dmar_fault(source_id, fault, addr, is_write);

    if (fsts_reg & VTD_FSTS_PFO) {
        error_report_once("New fault is not recorded due to "
                          "Primary Fault Overflow");
        return;
    }

    if (vtd_try_collapse_fault(s, source_id)) {
        error_report_once("New fault is not recorded due to "
                          "compression of faults");
        return;
    }

    if (vtd_is_frcd_set(s, s->next_frcd_reg)) {
        error_report_once("Next Fault Recording Reg is used, "
                          "new fault is not recorded, set PFO field");
        vtd_set_clear_mask_long(s, DMAR_FSTS_REG, 0, VTD_FSTS_PFO);
        return;
    }

    vtd_record_frcd(s, s->next_frcd_reg, source_id, addr, fault, is_write);

    if (fsts_reg & VTD_FSTS_PPF) {
        error_report_once("There are pending faults already, "
                          "fault event is not generated");
        vtd_set_frcd_and_update_ppf(s, s->next_frcd_reg);
        s->next_frcd_reg++;
        if (s->next_frcd_reg == DMAR_FRCD_REG_NR) {
            s->next_frcd_reg = 0;
        }
    } else {
        vtd_set_clear_mask_long(s, DMAR_FSTS_REG, VTD_FSTS_FRI_MASK,
                                VTD_FSTS_FRI(s->next_frcd_reg));
        vtd_set_frcd_and_update_ppf(s, s->next_frcd_reg); /* Will set PPF */
        s->next_frcd_reg++;
        if (s->next_frcd_reg == DMAR_FRCD_REG_NR) {
            s->next_frcd_reg = 0;
        }
        /* This case actually cause the PPF to be Set.
         * So generate fault event (interrupt).
         */
         vtd_generate_fault_event(s, fsts_reg);
    }
}

/* Handle Invalidation Queue Errors of queued invalidation interface error
 * conditions.
 */
static void vtd_handle_inv_queue_error(IntelIOMMUState *s)
{
    uint32_t fsts_reg = vtd_get_long_raw(s, DMAR_FSTS_REG);

    vtd_set_clear_mask_long(s, DMAR_FSTS_REG, 0, VTD_FSTS_IQE);
    vtd_generate_fault_event(s, fsts_reg);
}

/* Set the IWC field and try to generate an invalidation completion interrupt */
static void vtd_generate_completion_event(IntelIOMMUState *s)
{
    if (vtd_get_long_raw(s, DMAR_ICS_REG) & VTD_ICS_IWC) {
        trace_vtd_inv_desc_wait_irq("One pending, skip current");
        return;
    }
    vtd_set_clear_mask_long(s, DMAR_ICS_REG, 0, VTD_ICS_IWC);
    vtd_set_clear_mask_long(s, DMAR_IECTL_REG, 0, VTD_IECTL_IP);
    if (vtd_get_long_raw(s, DMAR_IECTL_REG) & VTD_IECTL_IM) {
        trace_vtd_inv_desc_wait_irq("IM in IECTL_REG is set, "
                                    "new event not generated");
        return;
    } else {
        /* Generate the interrupt event */
        trace_vtd_inv_desc_wait_irq("Generating complete event");
        vtd_generate_interrupt(s, DMAR_IEADDR_REG, DMAR_IEDATA_REG);
        vtd_set_clear_mask_long(s, DMAR_IECTL_REG, VTD_IECTL_IP, 0);
    }
}

static void vtd_generate_prq_event(IntelIOMMUState *s, uint32_t pre_prs)
{
    if (vtd_get_long_raw(s, DMAR_PRS_REG) & VTD_PRS_PPR) {
        trace_vtd_inv_desc_wait_irq("One pending, skip current");//YiLiu: TODO
        return;
    }
    vtd_set_clear_mask_long(s, DMAR_PRS_REG, 0, VTD_PRS_PPR);
    vtd_set_clear_mask_long(s, DMAR_PECTL_REG, 0, VTD_PECTL_IP);
    if (vtd_get_long_raw(s, DMAR_PECTL_REG) & VTD_PECTL_IM) {
        trace_vtd_inv_desc_wait_irq("IM in PECTL_REG is set, "
                                    "new event not generated");
        return;
    } else {
        trace_vtd_inv_desc_wait_irq("Generating PRQ event");
        vtd_generate_interrupt(s, DMAR_PEADDR_REG, DMAR_PEDATA_REG);
        vtd_set_clear_mask_long(s, DMAR_PECTL_REG, VTD_PECTL_IP, 0);
    }
}

/*
 * Handle PRQ Queue Errors
 */
static void vtd_handle_prq_queue_error(IntelIOMMUState *s)
{
    uint32_t fsts_reg = vtd_get_long_raw(s, DMAR_FSTS_REG);

    vtd_set_clear_mask_long(s, DMAR_FSTS_REG, 0, VTD_FSTS_IQE);
    vtd_generate_fault_event(s, fsts_reg);
}

/*
 * Enqueue a page request to software and generate interrupt
 */
static void vtd_report_page_request(IntelIOMMUState *s,
                                    VTDPageReqDsc *prq)
{
    uint32_t fsts_reg = vtd_get_long_raw(s, DMAR_FSTS_REG);
    uint32_t prs_reg = vtd_get_long_raw(s, DMAR_PRS_REG);
    dma_addr_t addr;

    //trace_vtd_dmar_fault(source_id, fault, addr, is_write);

    if (fsts_reg & VTD_FSTS_PRO) {
        //trace_vtd_err("New page request is not enqueued due to "
        //              "Page Request Overflow.");
        return;
    }

    if (s->prq_entry_count < s->prq_nb_entries) {
        prq->qw_0 = cpu_to_le64(prq->qw_0);
        prq->qw_1 = cpu_to_le64(prq->qw_1);
        prq->priv_data[0] = cpu_to_le64(prq->priv_data[0]);
        prq->priv_data[1] = cpu_to_le64(prq->priv_data[1]);
        addr = s->prq_tail + s->pqa;
        if (dma_memory_write(&address_space_memory, addr, prq,
                             sizeof(*prq))) {
            vtd_handle_prq_queue_error(s);
            return;
        }
        s->prq_tail = (s->prq_tail + sizeof(*prq)) % s->prq_qsize;
        vtd_set_long(s, DMAR_PQT_REG, s->prq_tail);// TODO: check if need to set quad
        s->prq_entry_count++;
        if (s->prq_entry_count == s->prq_nb_entries) {
            //trace_vtd_err("Page Request Queue is full, "
            //              "set PFO field.");
            //TODO: handle the overflow fault, also the PRO bit has been moved
            // to PRQ status register
            VTD_DEBUG("%s, s->prq_entry_count: %d full!!!!!\n", __func__, s->prq_entry_count);
            vtd_set_clear_mask_long(s, DMAR_FSTS_REG, 0, VTD_FSTS_PRO);
        }
    }

    if (!(prs_reg & VTD_PRS_PPR)) {
        // vtd_set_clear_mask_long(s, DMAR_PRS_REG, 0, VTD_PRS_PPR); set it after generating irq
        /*
         * This case actually cause the PPR to be Set.
         * So generate prq event (interrupt).
         */
         vtd_generate_prq_event(s, prs_reg);
    }
}

static inline bool vtd_root_entry_present(IntelIOMMUState *s,
                                          VTDRootEntry *re,
                                          uint8_t devfn)
{
    if (s->root_scalable && devfn > UINT8_MAX / 2) {
        return re->hi & VTD_ROOT_ENTRY_P;
    }

    return re->lo & VTD_ROOT_ENTRY_P;
}

static int vtd_get_root_entry(IntelIOMMUState *s, uint8_t index,
                              VTDRootEntry *re)
{
    dma_addr_t addr;

    addr = s->root + index * sizeof(*re);
    if (dma_memory_read(&address_space_memory, addr, re, sizeof(*re))) {
        re->lo = 0;
        return -VTD_FR_ROOT_TABLE_INV;
    }
    re->lo = le64_to_cpu(re->lo);
    re->hi = le64_to_cpu(re->hi);
    return 0;
}

static inline bool vtd_ce_present(VTDContextEntry *context)
{
    return context->lo & VTD_CONTEXT_ENTRY_P;
}

static int vtd_get_context_entry_from_root(IntelIOMMUState *s,
                                           VTDRootEntry *re,
                                           uint8_t index,
                                           VTDContextEntry *ce)
{
    dma_addr_t addr, ce_size;

    /* we have checked that root entry is present */
    ce_size = s->root_scalable ? VTD_CTX_ENTRY_SCALABLE_SIZE :
              VTD_CTX_ENTRY_LEGACY_SIZE;

    if (s->root_scalable && index > UINT8_MAX / 2) {
        index = index & (~VTD_DEVFN_CHECK_MASK);
        addr = re->hi & VTD_ROOT_ENTRY_CTP;
    } else {
        addr = re->lo & VTD_ROOT_ENTRY_CTP;
    }

    addr = addr + index * ce_size;
    if (dma_memory_read(&address_space_memory, addr, ce, ce_size)) {
        return -VTD_FR_CONTEXT_TABLE_INV;
    }

    ce->lo = le64_to_cpu(ce->lo);
    ce->hi = le64_to_cpu(ce->hi);
    if (ce_size == VTD_CTX_ENTRY_SCALABLE_SIZE) {
        ce->val[2] = le64_to_cpu(ce->val[2]);
        ce->val[3] = le64_to_cpu(ce->val[3]);
    }
    return 0;
}

static inline dma_addr_t vtd_ce_get_slpt_base(VTDContextEntry *ce)
{
    return ce->lo & VTD_CONTEXT_ENTRY_SLPTPTR;
}

static inline uint64_t vtd_get_slpte_addr(uint64_t slpte, uint8_t aw)
{
    return slpte & VTD_SL_PT_BASE_ADDR_MASK(aw);
}

/* Whether the pte indicates the address of the page frame */
static inline bool vtd_is_last_slpte(uint64_t slpte, uint32_t level)
{
    return level == VTD_SL_PT_LEVEL || (slpte & VTD_SL_PT_PAGE_SIZE_MASK);
}

/* Get the content of a spte located in @base_addr[@index] */
static uint64_t vtd_get_slpte(dma_addr_t base_addr, uint32_t index)
{
    uint64_t slpte;

    assert(index < VTD_SL_PT_ENTRY_NR);

    if (dma_memory_read(&address_space_memory,
                        base_addr + index * sizeof(slpte), &slpte,
                        sizeof(slpte))) {
        slpte = (uint64_t)-1;
        return slpte;
    }
    slpte = le64_to_cpu(slpte);
    return slpte;
}

/* Given an iova and the level of paging structure, return the offset
 * of current level.
 */
static inline uint32_t vtd_iova_level_offset(uint64_t iova, uint32_t level)
{
    return (iova >> vtd_slpt_level_shift(level)) &
            ((1ULL << VTD_SL_LEVEL_BITS) - 1);
}

/* Check Capability Register to see if the @level of page-table is supported */
static inline bool vtd_is_level_supported(IntelIOMMUState *s, uint32_t level)
{
    return VTD_CAP_SAGAW_MASK & s->cap &
           (1ULL << (level - 2 + VTD_CAP_SAGAW_SHIFT));
}

/* Return true if check passed, otherwise false */
static inline bool vtd_pe_type_check(X86IOMMUState *x86_iommu,
                                     VTDPASIDEntry *pe)
{
    switch (VTD_PE_GET_TYPE(pe)) {
    case VTD_SM_PASID_ENTRY_FLT:
    case VTD_SM_PASID_ENTRY_SLT:
    case VTD_SM_PASID_ENTRY_NESTED:
        break;
    case VTD_SM_PASID_ENTRY_PT:
        if (!x86_iommu->pt_supported) {
            return false;
        }
        break;
    default:
        /* Unknown type */
        return false;
    }
    return true;
}

static inline uint16_t vtd_pe_get_domain_id(VTDPASIDEntry *pe)
{
    return VTD_SM_PASID_ENTRY_DID((pe)->val[1]);
}

static inline uint32_t vtd_sm_ce_get_pdt_entry_num(VTDContextEntry *ce)
{
    return 1U << (VTD_SM_CONTEXT_ENTRY_PDTS(ce->val[0]) + 7);
}

static inline uint32_t vtd_pe_get_fl_aw(VTDPASIDEntry *pe)
{
    return 48 + ((pe->val[2] >> 2) & VTD_SM_PASID_ENTRY_FLPM) * 9;
}

static inline dma_addr_t vtd_pe_get_flpt_base(VTDPASIDEntry *pe)
{
    return pe->val[2] & VTD_SM_PASID_ENTRY_FLPTPTR;
}

static inline void pasid_cache_info_set_error(VTDPASIDCacheInfo *pc_info)
{
    if (pc_info->error_happened) {
        return;
    }
    pc_info->error_happened = true;
}

static inline bool vtd_pdire_present(VTDPASIDDirEntry *pdire)
{
    return pdire->val & 1;
}

/**
 * Caller of this function should check present bit if wants
 * to use pdir entry for further usage except for fpd bit check.
 */
static int vtd_get_pdire_from_pdir_table(dma_addr_t pasid_dir_base,
                                         uint32_t pasid,
                                         VTDPASIDDirEntry *pdire)
{
    uint32_t index;
    dma_addr_t addr, entry_size;

    index = VTD_PASID_DIR_INDEX(pasid);
    entry_size = VTD_PASID_DIR_ENTRY_SIZE;
    addr = pasid_dir_base + index * entry_size;
    if (dma_memory_read(&address_space_memory, addr, pdire, entry_size)) {
        return -VTD_FR_PASID_DIR_ACCESS_ERR;
    }

    return 0;
}

static inline bool vtd_pe_present(VTDPASIDEntry *pe)
{
    return pe->val[0] & VTD_PASID_ENTRY_P;
}

static inline uint32_t vtd_pe_get_flpt_level(VTDPASIDEntry *pe)
{
    return (4 + ((pe->val[2] >> 2) & VTD_SM_PASID_ENTRY_FLPM));
}

/* Check if the first level paging mode is supported */
static inline bool vtd_is_flpm_supported(IntelIOMMUState *s, uint32_t level)
{
    if (level != 4 && level != 5)
        return false;

    if (level == 5)
        return !!(s->cap & VTD_CAP_FL5LP);

    return true;
}

static int vtd_get_pe_in_pasid_leaf_table(IntelIOMMUState *s,
                                          uint32_t pasid,
                                          dma_addr_t addr,
                                          VTDPASIDEntry *pe)
{
    uint32_t index;
    dma_addr_t entry_size;
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);
    uint8_t pgtt;

    index = VTD_PASID_TABLE_INDEX(pasid);
    entry_size = VTD_PASID_ENTRY_SIZE;
    addr = addr + index * entry_size;
    if (dma_memory_read(&address_space_memory, addr, pe, entry_size)) {
        return -VTD_FR_PASID_TABLE_ACCESS_ERR;
    }

    /* Do translation type check */
    if (!vtd_pe_type_check(x86_iommu, pe)) {
        return -VTD_FR_PASID_TABLE_ENTRY_INV;
    }

    pgtt = VTD_PE_GET_TYPE(pe);
    if (pgtt == VTD_SM_PASID_ENTRY_SLT &&
        !vtd_is_level_supported(s, VTD_PE_GET_LEVEL(pe)))
            return -VTD_FR_PASID_TABLE_ENTRY_INV;

    if (pgtt == VTD_SM_PASID_ENTRY_FLT &&
        !vtd_is_flpm_supported(s, vtd_pe_get_flpt_level(pe))) {
            return -VTD_FR_PASID_TABLE_ENTRY_INV;
    }
    return 0;
}

/**
 * Caller of this function should check present bit if wants
 * to use pasid entry for further usage except for fpd bit check.
 */
static int vtd_get_pe_from_pdire(IntelIOMMUState *s,
                                 uint32_t pasid,
                                 VTDPASIDDirEntry *pdire,
                                 VTDPASIDEntry *pe)
{
    dma_addr_t addr = pdire->val & VTD_PASID_TABLE_BASE_ADDR_MASK;

    return vtd_get_pe_in_pasid_leaf_table(s, pasid, addr, pe);
}

/**
 * This function gets a pasid entry from a specified pasid
 * table (includes dir and leaf table) with a specified pasid.
 * Sanity check should be done to ensure return a present
 * pasid entry to caller.
 */
static int vtd_get_pe_from_pasid_table(IntelIOMMUState *s,
                                       dma_addr_t pasid_dir_base,
                                       uint32_t pasid,
                                       VTDPASIDEntry *pe)
{
    int ret;
    VTDPASIDDirEntry pdire;

    ret = vtd_get_pdire_from_pdir_table(pasid_dir_base,
                                        pasid, &pdire);
    if (ret) {
        return ret;
    }

    if (!vtd_pdire_present(&pdire)) {
        return -VTD_FR_PASID_DIR_ENTRY_P;
    }

    ret = vtd_get_pe_from_pdire(s, pasid, &pdire, pe);
    if (ret) {
        return ret;
    }

    if (!vtd_pe_present(pe)) {
        return -VTD_FR_PASID_ENTRY_P;
    }

    return 0;
}

static int vtd_ce_get_rid2pasid_entry(IntelIOMMUState *s,
                                      VTDContextEntry *ce,
                                      VTDPASIDEntry *pe)
{
    uint32_t pasid;
    dma_addr_t pasid_dir_base;
    int ret = 0;

    pasid = VTD_CE_GET_RID2PASID(ce);
    pasid_dir_base = VTD_CE_GET_PASID_DIR_TABLE(ce);
    ret = vtd_get_pe_from_pasid_table(s, pasid_dir_base, pasid, pe);

    return ret;
}

static int vtd_ce_get_pasid_fpd(IntelIOMMUState *s,
                                VTDContextEntry *ce,
                                bool *pe_fpd_set)
{
    int ret;
    uint32_t pasid;
    dma_addr_t pasid_dir_base;
    VTDPASIDDirEntry pdire;
    VTDPASIDEntry pe;

    pasid = VTD_CE_GET_RID2PASID(ce);
    pasid_dir_base = VTD_CE_GET_PASID_DIR_TABLE(ce);

    /*
     * No present bit check since fpd is meaningful even
     * if the present bit is clear.
     */
    ret = vtd_get_pdire_from_pdir_table(pasid_dir_base, pasid, &pdire);
    if (ret) {
        return ret;
    }

    if (pdire.val & VTD_PASID_DIR_FPD) {
        *pe_fpd_set = true;
        return 0;
    }

    if (!vtd_pdire_present(&pdire)) {
        return -VTD_FR_PASID_DIR_ENTRY_P;
    }

    /*
     * No present bit check since fpd is meaningful even
     * if the present bit is clear.
     */
    ret = vtd_get_pe_from_pdire(s, pasid, &pdire, &pe);
    if (ret) {
        return ret;
    }

    if (pe.val[0] & VTD_PASID_ENTRY_FPD) {
        *pe_fpd_set = true;
    }

    return 0;
}

/* Get the page-table level that hardware should use for the second-level
 * page-table walk from the Address Width field of context-entry.
 */
static inline uint32_t vtd_ce_get_level(VTDContextEntry *ce)
{
    return 2 + (ce->hi & VTD_CONTEXT_ENTRY_AW);
}

static uint32_t vtd_get_iova_level(IntelIOMMUState *s,
                                   VTDContextEntry *ce)
{
    VTDPASIDEntry pe;

    if (s->root_scalable) {
        vtd_ce_get_rid2pasid_entry(s, ce, &pe);
        return VTD_PE_GET_LEVEL(&pe);
    }

    return vtd_ce_get_level(ce);
}

static inline uint32_t vtd_ce_get_agaw(VTDContextEntry *ce)
{
    return 30 + (ce->hi & VTD_CONTEXT_ENTRY_AW) * 9;
}

static uint32_t vtd_get_iova_agaw(IntelIOMMUState *s,
                                  VTDContextEntry *ce)
{
    VTDPASIDEntry pe;

    if (s->root_scalable) {
        vtd_ce_get_rid2pasid_entry(s, ce, &pe);
        return 30 + ((pe.val[0] >> 2) & VTD_SM_PASID_ENTRY_AW) * 9;
    }

    return vtd_ce_get_agaw(ce);
}

static inline uint32_t vtd_ce_get_type(VTDContextEntry *ce)
{
    return ce->lo & VTD_CONTEXT_ENTRY_TT;
}

/* Only for Legacy Mode. Return true if check passed, otherwise false */
static inline bool vtd_ce_type_check(X86IOMMUState *x86_iommu,
                                     VTDContextEntry *ce)
{
    switch (vtd_ce_get_type(ce)) {
    case VTD_CONTEXT_TT_MULTI_LEVEL:
        /* Always supported */
        break;
    case VTD_CONTEXT_TT_DEV_IOTLB:
        if (!x86_iommu->dt_supported) {
            error_report_once("%s: DT specified but not supported", __func__);
            return false;
        }
        break;
    case VTD_CONTEXT_TT_PASS_THROUGH:
        if (!x86_iommu->pt_supported) {
            error_report_once("%s: PT specified but not supported", __func__);
            return false;
        }
        break;
    default:
        /* Unknown type */
        error_report_once("%s: unknown ce type: %"PRIu32, __func__,
                          vtd_ce_get_type(ce));
        return false;
    }
    return true;
}

static inline uint64_t vtd_iova_limit(IntelIOMMUState *s,
                                      VTDContextEntry *ce, uint8_t aw)
{
    uint32_t ce_agaw = vtd_get_iova_agaw(s, ce);
    return 1ULL << MIN(ce_agaw, aw);
}

/* Return true if IOVA passes range check, otherwise false. */
static inline bool vtd_iova_range_check(IntelIOMMUState *s,
                                        uint64_t iova, VTDContextEntry *ce,
                                        uint8_t aw)
{
    /*
     * Check if @iova is above 2^X-1, where X is the minimum of MGAW
     * in CAP_REG and AW in context-entry.
     */
    return !(iova & ~(vtd_iova_limit(s, ce, aw) - 1));
}

static dma_addr_t vtd_get_iova_pgtbl_base(IntelIOMMUState *s,
                                          VTDContextEntry *ce)
{
    VTDPASIDEntry pe;

    if (s->root_scalable) {
        vtd_ce_get_rid2pasid_entry(s, ce, &pe);
        return pe.val[0] & VTD_SM_PASID_ENTRY_SLPTPTR;
    }

    return vtd_ce_get_slpt_base(ce);
}

/*
 * Rsvd field masks for spte:
 *     vtd_spte_rsvd 4k pages
 *     vtd_spte_rsvd_large large pages
 */
static uint64_t vtd_spte_rsvd[5];
static uint64_t vtd_spte_rsvd_large[5];

static bool vtd_slpte_nonzero_rsvd(uint64_t slpte, uint32_t level)
{
    uint64_t rsvd_mask = vtd_spte_rsvd[level];

    if ((level == VTD_SL_PD_LEVEL || level == VTD_SL_PDP_LEVEL) &&
        (slpte & VTD_SL_PT_PAGE_SIZE_MASK)) {
        /* large page */
        rsvd_mask = vtd_spte_rsvd_large[level];
    }

    return slpte & rsvd_mask;
}

/* Find the VTD address space associated with a given bus number */
static VTDBus *vtd_find_as_from_bus_num(IntelIOMMUState *s, uint8_t bus_num)
{
    VTDBus *vtd_bus = s->vtd_as_by_bus_num[bus_num];
    GHashTableIter iter;

    if (vtd_bus) {
        return vtd_bus;
    }

    /*
     * Iterate over the registered buses to find the one which
     * currently holds this bus number and update the bus_num
     * lookup table.
     */
    g_hash_table_iter_init(&iter, s->vtd_as_by_busptr);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&vtd_bus)) {
        if (pci_bus_num(vtd_bus->bus) == bus_num) {
            s->vtd_as_by_bus_num[bus_num] = vtd_bus;
            return vtd_bus;
        }
    }

    return NULL;
}

/* Given the @iova, get relevant @slptep. @slpte_level will be the last level
 * of the translation, can be used for deciding the size of large page.
 */
static int vtd_iova_to_slpte(IntelIOMMUState *s, VTDContextEntry *ce,
                             uint64_t iova, bool is_write,
                             uint64_t *slptep, uint32_t *slpte_level,
                             bool *reads, bool *writes, uint8_t aw_bits)
{
    dma_addr_t addr = vtd_get_iova_pgtbl_base(s, ce);
    uint32_t level = vtd_get_iova_level(s, ce);
    uint32_t offset;
    uint64_t slpte;
    uint64_t access_right_check;

    if (!vtd_iova_range_check(s, iova, ce, aw_bits)) {
        error_report_once("%s: detected IOVA overflow (iova=0x%" PRIx64 ")",
                          __func__, iova);
        return -VTD_FR_ADDR_BEYOND_MGAW;
    }

    /* FIXME: what is the Atomics request here? */
    access_right_check = is_write ? VTD_SL_W : VTD_SL_R;

    while (true) {
        offset = vtd_iova_level_offset(iova, level);
        slpte = vtd_get_slpte(addr, offset);

        if (slpte == (uint64_t)-1) {
            error_report_once("%s: detected read error on DMAR slpte "
                              "(iova=0x%" PRIx64 ")", __func__, iova);
            if (level == vtd_get_iova_level(s, ce)) {
                /* Invalid programming of context-entry */
                return -VTD_FR_CONTEXT_ENTRY_INV;
            } else {
                return -VTD_FR_PAGING_ENTRY_INV;
            }
        }
        *reads = (*reads) && (slpte & VTD_SL_R);
        *writes = (*writes) && (slpte & VTD_SL_W);
        if (!(slpte & access_right_check)) {
            error_report_once("%s: detected slpte permission error "
                              "(iova=0x%" PRIx64 ", level=0x%" PRIx32 ", "
                              "slpte=0x%" PRIx64 ", write=%d)", __func__,
                              iova, level, slpte, is_write);
            return is_write ? -VTD_FR_WRITE : -VTD_FR_READ;
        }
        if (vtd_slpte_nonzero_rsvd(slpte, level)) {
            error_report_once("%s: detected splte reserve non-zero "
                              "iova=0x%" PRIx64 ", level=0x%" PRIx32
                              "slpte=0x%" PRIx64 ")", __func__, iova,
                              level, slpte);
            return -VTD_FR_PAGING_ENTRY_RSVD;
        }

        if (vtd_is_last_slpte(slpte, level)) {
            *slptep = slpte;
            *slpte_level = level;
            return 0;
        }
        addr = vtd_get_slpte_addr(slpte, aw_bits);
        level--;
    }
}

typedef int (*vtd_page_walk_hook)(IOMMUTLBEvent *event, void *private);

/**
 * Constant information used during page walking
 *
 * @hook_fn: hook func to be called when detected page
 * @private: private data to be passed into hook func
 * @notify_unmap: whether we should notify invalid entries
 * @as: VT-d address space of the device
 * @aw: maximum address width
 * @domain: domain ID of the page walk
 */
typedef struct {
    VTDAddressSpace *as;
    vtd_page_walk_hook hook_fn;
    void *private;
    bool notify_unmap;
    uint8_t aw;
    uint16_t domain_id;
} vtd_page_walk_info;

static int vtd_page_walk_one(IOMMUTLBEvent *event, vtd_page_walk_info *info)
{
    VTDAddressSpace *as = info->as;
    vtd_page_walk_hook hook_fn = info->hook_fn;
    void *private = info->private;
    IOMMUTLBEntry *entry = &event->entry;
    DMAMap target = {
        .iova = entry->iova,
        .size = entry->addr_mask,
        .translated_addr = entry->translated_addr,
        .perm = entry->perm,
    };
    DMAMap *mapped = iova_tree_find(as->iova_tree, &target);

    if (event->type == IOMMU_NOTIFIER_UNMAP && !info->notify_unmap) {
        trace_vtd_page_walk_one_skip_unmap(entry->iova, entry->addr_mask);
        return 0;
    }

    assert(hook_fn);

    /* Update local IOVA mapped ranges */
    if (event->type == IOMMU_NOTIFIER_MAP) {
        if (mapped) {
            /* If it's exactly the same translation, skip */
            if (!memcmp(mapped, &target, sizeof(target))) {
                trace_vtd_page_walk_one_skip_map(entry->iova, entry->addr_mask,
                                                 entry->translated_addr);
                goto do_map;
            } else {
                /*
                 * Translation changed.  Normally this should not
                 * happen, but it can happen when with buggy guest
                 * OSes.  Note that there will be a small window that
                 * we don't have map at all.  But that's the best
                 * effort we can do.  The ideal way to emulate this is
                 * atomically modify the PTE to follow what has
                 * changed, but we can't.  One example is that vfio
                 * driver only has VFIO_IOMMU_[UN]MAP_DMA but no
                 * interface to modify a mapping (meanwhile it seems
                 * meaningless to even provide one).  Anyway, let's
                 * mark this as a TODO in case one day we'll have
                 * a better solution.
                 */
                IOMMUAccessFlags cache_perm = entry->perm;
                int ret;

                /* Emulate an UNMAP */
                event->type = IOMMU_NOTIFIER_UNMAP;
                entry->perm = IOMMU_NONE;
                trace_vtd_page_walk_one(info->domain_id,
                                        entry->iova,
                                        entry->translated_addr,
                                        entry->addr_mask,
                                        entry->perm);
                ret = hook_fn(event, private);
                if (ret) {
                    return ret;
                }
                /* Drop any existing mapping */
                iova_tree_remove(as->iova_tree, &target);
                /* Recover the correct type */
                event->type = IOMMU_NOTIFIER_MAP;
                entry->perm = cache_perm;
            }
        }
        iova_tree_insert(as->iova_tree, &target);
    } else {
        if (!mapped) {
            /* Skip since we didn't map this range at all */
            trace_vtd_page_walk_one_skip_unmap(entry->iova, entry->addr_mask);
            return 0;
        }
        iova_tree_remove(as->iova_tree, &target);
    }

    trace_vtd_page_walk_one(info->domain_id, entry->iova,
                            entry->translated_addr, entry->addr_mask,
                            entry->perm);
do_map:
    return hook_fn(event, private);
}

/**
 * vtd_page_walk_level - walk over specific level for IOVA range
 *
 * @addr: base GPA addr to start the walk
 * @start: IOVA range start address
 * @end: IOVA range end address (start <= addr < end)
 * @read: whether parent level has read permission
 * @write: whether parent level has write permission
 * @info: constant information for the page walk
 */
static int vtd_page_walk_level(dma_addr_t addr, uint64_t start,
                               uint64_t end, uint32_t level, bool read,
                               bool write, vtd_page_walk_info *info)
{
    bool read_cur, write_cur, entry_valid;
    uint32_t offset;
    uint64_t slpte;
    uint64_t subpage_size, subpage_mask;
    IOMMUTLBEvent event;
    uint64_t iova = start;
    uint64_t iova_next;
    int ret = 0;

    trace_vtd_page_walk_level(addr, level, start, end);

    subpage_size = 1ULL << vtd_slpt_level_shift(level);
    subpage_mask = vtd_slpt_level_page_mask(level);

    while (iova < end) {
        iova_next = (iova & subpage_mask) + subpage_size;

        offset = vtd_iova_level_offset(iova, level);
        slpte = vtd_get_slpte(addr, offset);

        if (slpte == (uint64_t)-1) {
            trace_vtd_page_walk_skip_read(iova, iova_next);
            goto next;
        }

        if (vtd_slpte_nonzero_rsvd(slpte, level)) {
            trace_vtd_page_walk_skip_reserve(iova, iova_next);
            goto next;
        }

        /* Permissions are stacked with parents' */
        read_cur = read && (slpte & VTD_SL_R);
        write_cur = write && (slpte & VTD_SL_W);

        /*
         * As long as we have either read/write permission, this is a
         * valid entry. The rule works for both page entries and page
         * table entries.
         */
        entry_valid = read_cur | write_cur;

        if (!vtd_is_last_slpte(slpte, level) && entry_valid) {
            /*
             * This is a valid PDE (or even bigger than PDE).  We need
             * to walk one further level.
             */
            ret = vtd_page_walk_level(vtd_get_slpte_addr(slpte, info->aw),
                                      iova, MIN(iova_next, end), level - 1,
                                      read_cur, write_cur, info);
        } else {
            /*
             * This means we are either:
             *
             * (1) the real page entry (either 4K page, or huge page)
             * (2) the whole range is invalid
             *
             * In either case, we send an IOTLB notification down.
             */
            event.entry.target_as = &address_space_memory;
            event.entry.iova = iova & subpage_mask;
            event.entry.perm = IOMMU_ACCESS_FLAG(read_cur, write_cur);
            event.entry.addr_mask = ~subpage_mask;
            /* NOTE: this is only meaningful if entry_valid == true */
            event.entry.translated_addr = vtd_get_slpte_addr(slpte, info->aw);
            event.type = event.entry.perm ? IOMMU_NOTIFIER_MAP :
                                            IOMMU_NOTIFIER_UNMAP;
            ret = vtd_page_walk_one(&event, info);
        }

        if (ret < 0) {
            return ret;
        }

next:
        iova = iova_next;
    }

    return 0;
}

/**
 * vtd_page_walk - walk specific IOVA range, and call the hook
 *
 * @s: intel iommu state
 * @ce: context entry to walk upon
 * @start: IOVA address to start the walk
 * @end: IOVA range end address (start <= addr < end)
 * @info: page walking information struct
 */
static int vtd_page_walk(IntelIOMMUState *s, VTDContextEntry *ce,
                         uint64_t start, uint64_t end,
                         vtd_page_walk_info *info)
{
    dma_addr_t addr = vtd_get_iova_pgtbl_base(s, ce);
    uint32_t level = vtd_get_iova_level(s, ce);

    if (!vtd_iova_range_check(s, start, ce, info->aw)) {
        return -VTD_FR_ADDR_BEYOND_MGAW;
    }

    if (!vtd_iova_range_check(s, end, ce, info->aw)) {
        /* Fix end so that it reaches the maximum */
        end = vtd_iova_limit(s, ce, info->aw);
    }

    return vtd_page_walk_level(addr, start, end, level, true, true, info);
}

static int vtd_root_entry_rsvd_bits_check(IntelIOMMUState *s,
                                          VTDRootEntry *re)
{
    /* Legacy Mode reserved bits check */
    if (!s->root_scalable &&
        (re->hi || (re->lo & VTD_ROOT_ENTRY_RSVD(s->aw_bits))))
        goto rsvd_err;

    /* Scalable Mode reserved bits check */
    if (s->root_scalable &&
        ((re->lo & VTD_ROOT_ENTRY_RSVD(s->aw_bits)) ||
         (re->hi & VTD_ROOT_ENTRY_RSVD(s->aw_bits))))
        goto rsvd_err;

    return 0;

rsvd_err:
    error_report_once("%s: invalid root entry: hi=0x%"PRIx64
                      ", lo=0x%"PRIx64,
                      __func__, re->hi, re->lo);
    return -VTD_FR_ROOT_ENTRY_RSVD;
}

static inline int vtd_context_entry_rsvd_bits_check(IntelIOMMUState *s,
                                                    VTDContextEntry *ce)
{
    if (!s->root_scalable &&
        (ce->hi & VTD_CONTEXT_ENTRY_RSVD_HI ||
         ce->lo & VTD_CONTEXT_ENTRY_RSVD_LO(s->aw_bits))) {
        error_report_once("%s: invalid context entry: hi=%"PRIx64
                          ", lo=%"PRIx64" (reserved nonzero)",
                          __func__, ce->hi, ce->lo);
        return -VTD_FR_CONTEXT_ENTRY_RSVD;
    }

    if (s->root_scalable &&
        (ce->val[0] & VTD_SM_CONTEXT_ENTRY_RSVD_VAL0(s->aw_bits) ||
         ce->val[1] & VTD_SM_CONTEXT_ENTRY_RSVD_VAL1 ||
         ce->val[2] ||
         ce->val[3])) {
        error_report_once("%s: invalid context entry: val[3]=%"PRIx64
                          ", val[2]=%"PRIx64
                          ", val[1]=%"PRIx64
                          ", val[0]=%"PRIx64" (reserved nonzero)",
                          __func__, ce->val[3], ce->val[2],
                          ce->val[1], ce->val[0]);
        return -VTD_FR_CONTEXT_ENTRY_RSVD;
    }

    return 0;
}

static int vtd_ce_rid2pasid_check(IntelIOMMUState *s,
                                  VTDContextEntry *ce)
{
    VTDPASIDEntry pe;

    /*
     * Make sure in Scalable Mode, a present context entry
     * has valid rid2pasid setting, which includes valid
     * rid2pasid field and corresponding pasid entry setting
     */
    return vtd_ce_get_rid2pasid_entry(s, ce, &pe);
}

/* Map a device to its corresponding domain (context-entry) */
static int vtd_dev_to_context_entry(IntelIOMMUState *s, uint8_t bus_num,
                                    uint8_t devfn, VTDContextEntry *ce)
{
    VTDRootEntry re;
    int ret_fr;
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);

    ret_fr = vtd_get_root_entry(s, bus_num, &re);
    if (ret_fr) {
        return ret_fr;
    }

    if (!vtd_root_entry_present(s, &re, devfn)) {
        /* Not error - it's okay we don't have root entry. */
        trace_vtd_re_not_present(bus_num);
        return -VTD_FR_ROOT_ENTRY_P;
    }

    ret_fr = vtd_root_entry_rsvd_bits_check(s, &re);
    if (ret_fr) {
        return ret_fr;
    }

    ret_fr = vtd_get_context_entry_from_root(s, &re, devfn, ce);
    if (ret_fr) {
        return ret_fr;
    }

    if (!vtd_ce_present(ce)) {
        /* Not error - it's okay we don't have context entry. */
        trace_vtd_ce_not_present(bus_num, devfn);
        return -VTD_FR_CONTEXT_ENTRY_P;
    }

    ret_fr = vtd_context_entry_rsvd_bits_check(s, ce);
    if (ret_fr) {
        return ret_fr;
    }

    /* Check if the programming of context-entry is valid */
    if (!s->root_scalable &&
        !vtd_is_level_supported(s, vtd_ce_get_level(ce))) {
        error_report_once("%s: invalid context entry: hi=%"PRIx64
                          ", lo=%"PRIx64" (level %d not supported)",
                          __func__, ce->hi, ce->lo,
                          vtd_ce_get_level(ce));
        return -VTD_FR_CONTEXT_ENTRY_INV;
    }

    if (!s->root_scalable) {
        /* Do translation type check */
        if (!vtd_ce_type_check(x86_iommu, ce)) {
            /* Errors dumped in vtd_ce_type_check() */
            return -VTD_FR_CONTEXT_ENTRY_INV;
        }
    } else {
        /*
         * Check if the programming of context-entry.rid2pasid
         * and corresponding pasid setting is valid, and thus
         * avoids to check pasid entry fetching result in future
         * helper function calling.
         */
        ret_fr = vtd_ce_rid2pasid_check(s, ce);
        if (ret_fr) {
            return ret_fr;
        }
    }

    return 0;
}

static int vtd_sync_shadow_page_hook(IOMMUTLBEvent *event,
                                     void *private)
{
    memory_region_notify_iommu(private, 0, *event);
    return 0;
}

static uint16_t vtd_get_domain_id(IntelIOMMUState *s,
                                  VTDContextEntry *ce)
{
    VTDPASIDEntry pe;

    if (s->root_scalable) {
        vtd_ce_get_rid2pasid_entry(s, ce, &pe);
        return VTD_SM_PASID_ENTRY_DID(pe.val[1]);
    }

    return VTD_CONTEXT_ENTRY_DID(ce->hi);
}

static int vtd_sync_shadow_page_table_range(VTDAddressSpace *vtd_as,
                                            VTDContextEntry *ce,
                                            hwaddr addr, hwaddr size)
{
    IntelIOMMUState *s = vtd_as->iommu_state;
    vtd_page_walk_info info = {
        .hook_fn = vtd_sync_shadow_page_hook,
        .private = (void *)&vtd_as->iommu,
        .notify_unmap = true,
        .aw = s->aw_bits,
        .as = vtd_as,
        .domain_id = vtd_get_domain_id(s, ce),
    };

    return vtd_page_walk(s, ce, addr, addr + size, &info);
}

static int vtd_sync_shadow_page_table(VTDAddressSpace *vtd_as)
{
    int ret;
    VTDContextEntry ce;
    IOMMUNotifier *n;
    struct iommu_page_response pg_resp;
    VTDPRQEntry *vtd_prq, *tmp;
    IntelIOMMUState *s = vtd_as->iommu_state;

    if (s->scalable_modern && s->root_scalable)
        return 0;

    if (!(vtd_as->iommu.iommu_notify_flags & IOMMU_NOTIFIER_IOTLB_EVENTS)) {
        return 0;
    }

    ret = vtd_dev_to_context_entry(vtd_as->iommu_state,
                                   pci_bus_num(vtd_as->bus),
                                   vtd_as->devfn, &ce);
    if (ret) {
        if (ret == -VTD_FR_CONTEXT_ENTRY_P) {
            /*
             * It's a valid scenario to have a context entry that is
             * not present.  For example, when a device is removed
             * from an existing domain then the context entry will be
             * zeroed by the guest before it was put into another
             * domain.  When this happens, instead of synchronizing
             * the shadow pages we should invalidate all existing
             * mappings and notify the backends.
             */
            IOMMU_NOTIFIER_FOREACH(n, &vtd_as->iommu) {
                vtd_address_space_unmap(vtd_as, n);
            }
            ret = 0;
        }
        return ret;
    }

    ret = vtd_sync_shadow_page_table_range(vtd_as, &ce, 0, UINT64_MAX);
    if (ret)
        return ret;

    /* If PRE bit of ce is disabled, we should send INVALID response */
    if (!(ce.val[0] & (1ULL << 4))) {
        ret = 0;

        VTD_DEBUG("%s: ce PRE bit is 0, prepare to submit INVALID grp resp.\n", __func__);
        qemu_mutex_lock(&s->prq_lock);
        QLIST_FOREACH_SAFE(vtd_prq, &s->vtd_prq_list, next, tmp) {
            vtd_assemble_pg_resp(&pg_resp, vtd_prq->prq, QI_RESP_INVALID);
            if (vtd_dev_send_page_response(s, vtd_as->bus, vtd_as->devfn, &pg_resp)) {
                error_report_once("%s: page response failed, resp_desc: "
                          "pasid=%d, flag=%x, code=%d", __func__,
                          pg_resp.pasid, pg_resp.flags, pg_resp.code);
                ret = -EINVAL;
                break;
            } else {
                QLIST_REMOVE(vtd_prq, next);
                g_free(vtd_prq);
                VTD_DEBUG("%s: successfully submit INVALID grp resp.\n", __func__);
            }
        }
        qemu_mutex_unlock(&s->prq_lock);
    }

    return ret;
}

static bool vtd_pe_pt_enabled(VTDPASIDEntry *pe)
{
    return (VTD_PE_GET_TYPE(pe) == VTD_SM_PASID_ENTRY_PT);
}

/*
 * Check if specific device is configured to bypass address
 * translation for DMA requests. In Scalable Mode, bypass
 * 1st-level translation or 2nd-level translation, it depends
 * on PGTT setting.
 */
static bool vtd_dev_pt_enabled(VTDAddressSpace *as)
{
    IntelIOMMUState *s;
    VTDContextEntry ce;
    VTDPASIDEntry pe;
    int ret;

    assert(as);

    s = as->iommu_state;
    ret = vtd_dev_to_context_entry(s, pci_bus_num(as->bus),
                                   as->devfn, &ce);
    if (ret) {
        /*
         * Possibly failed to parse the context entry for some reason
         * (e.g., during init, or any guest configuration errors on
         * context entries). We should assume PT not enabled for
         * safety.
         */
        return false;
    }

    if (s->root_scalable) {
        ret = vtd_ce_get_rid2pasid_entry(s, &ce, &pe);
        if (ret) {
            error_report_once("%s: vtd_ce_get_rid2pasid_entry error: %"PRId32,
                              __func__, ret);
            return false;
        }
        return vtd_pe_pt_enabled(&pe);
    }

    return (vtd_ce_get_type(&ce) == VTD_CONTEXT_TT_PASS_THROUGH);
}

/* Return whether the device is using IOMMU translation. */
static bool vtd_switch_address_space(VTDAddressSpace *as)
{
    bool use_iommu;
    /* Whether we need to take the BQL on our own */
    bool take_bql = !qemu_mutex_iothread_locked();
    IntelIOMMUState *s = as->iommu_state;
    VTDContextEntry ce;
    VTDPASIDEntry pe;
    int ret = 0;
    VTDBus *vtd_bus;
    VTDHostIOMMUContext *vtd_dev_icx;

    assert(as);

    use_iommu = s->dmar_enabled && !vtd_dev_pt_enabled(as);

    trace_vtd_switch_address_space(pci_bus_num(as->bus),
                                   VTD_PCI_SLOT(as->devfn),
                                   VTD_PCI_FUNC(as->devfn),
                                   use_iommu);

    /*
     * It's possible that we reach here without BQL, e.g., when called
     * from vtd_pt_enable_fast_path(). However the memory APIs need
     * it. We'd better make sure we have had it already, or, take it.
     */
    if (take_bql) {
        qemu_mutex_lock_iothread();
    }

    /* For passthrough device, we don't switch as */
    vtd_bus = vtd_find_add_bus(s, as->bus);
    vtd_dev_icx = vtd_bus->dev_icx[as->devfn];
    if (s->root_scalable && likely(s->dmar_enabled) && vtd_dev_icx) {
        ret = vtd_dev_to_context_entry(s, pci_bus_num(as->bus),
                                   as->devfn, &ce);
        if (!ret) {
            ret = vtd_ce_get_rid2pasid_entry(s, &ce, &pe);
            if (!ret && (VTD_PE_GET_TYPE(&pe) == VTD_SM_PASID_ENTRY_FLT)) {
                use_iommu = false;
            }
        }
        if (ret) {
            error_report_once("%s: cannot find ctx or pe for "
                              "(dev=%02x:%02x:%02x)",
                              __func__, pci_bus_num(as->bus),
                              VTD_PCI_SLOT(as->devfn),
                              VTD_PCI_FUNC(as->devfn));
            return false;
        }
    }

    /* Turn off first then on the other */
    if (use_iommu) {
        memory_region_set_enabled(&as->nodmar, false);
        memory_region_set_enabled(MEMORY_REGION(&as->iommu), true);
    } else {
        memory_region_set_enabled(MEMORY_REGION(&as->iommu), false);
        memory_region_set_enabled(&as->nodmar, true);
    }

    if (take_bql) {
        qemu_mutex_unlock_iothread();
    }

    return use_iommu;
}

static void vtd_switch_address_space_all(IntelIOMMUState *s)
{
    GHashTableIter iter;
    VTDBus *vtd_bus;
    int i;

    g_hash_table_iter_init(&iter, s->vtd_as_by_busptr);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&vtd_bus)) {
        for (i = 0; i < PCI_DEVFN_MAX; i++) {
            if (!vtd_bus->dev_as[i]) {
                continue;
            }
            vtd_switch_address_space(vtd_bus->dev_as[i]);
        }
    }
}

static inline uint16_t vtd_make_source_id(uint8_t bus_num, uint8_t devfn)
{
    return ((bus_num & 0xffUL) << 8) | (devfn & 0xffUL);
}

static const bool vtd_qualified_faults[] = {
    [VTD_FR_RESERVED] = false,
    [VTD_FR_ROOT_ENTRY_P] = false,
    [VTD_FR_CONTEXT_ENTRY_P] = true,
    [VTD_FR_CONTEXT_ENTRY_INV] = true,
    [VTD_FR_ADDR_BEYOND_MGAW] = true,
    [VTD_FR_WRITE] = true,
    [VTD_FR_READ] = true,
    [VTD_FR_PAGING_ENTRY_INV] = true,
    [VTD_FR_ROOT_TABLE_INV] = false,
    [VTD_FR_CONTEXT_TABLE_INV] = false,
    [VTD_FR_ROOT_ENTRY_RSVD] = false,
    [VTD_FR_PAGING_ENTRY_RSVD] = true,
    [VTD_FR_CONTEXT_ENTRY_TT] = true,
    [VTD_FR_PASID_DIR_ACCESS_ERR] = false,
    [VTD_FR_PASID_DIR_ENTRY_P] = true,
    [VTD_FR_PASID_TABLE_ACCESS_ERR] = false,
    [VTD_FR_PASID_ENTRY_P] = true,
    [VTD_FR_PASID_TABLE_ENTRY_INV] = true,
    [VTD_FR_ADDR_NONCANONICAL] = true,
    [VTD_FR_RESERVED_ERR] = false,
    [VTD_FR_MAX] = false,
};

/* To see if a fault condition is "qualified", which is reported to software
 * only if the FPD field in the context-entry used to process the faulting
 * request is 0.
 */
static inline bool vtd_is_qualified_fault(VTDFaultReason fault)
{
    return vtd_qualified_faults[fault];
}

static inline bool vtd_is_interrupt_addr(hwaddr addr)
{
    return VTD_INTERRUPT_ADDR_FIRST <= addr && addr <= VTD_INTERRUPT_ADDR_LAST;
}

static void vtd_pt_enable_fast_path(IntelIOMMUState *s, uint16_t source_id)
{
    VTDBus *vtd_bus;
    VTDAddressSpace *vtd_as;
    bool success = false;

    vtd_bus = vtd_find_as_from_bus_num(s, VTD_SID_TO_BUS(source_id));
    if (!vtd_bus) {
        goto out;
    }

    vtd_as = vtd_bus->dev_as[VTD_SID_TO_DEVFN(source_id)];
    if (!vtd_as) {
        goto out;
    }

    if (vtd_switch_address_space(vtd_as) == false) {
        /* We switched off IOMMU region successfully. */
        success = true;
    }

out:
    trace_vtd_pt_enable_fast_path(source_id, success);
}

/* The shift of an addr for a certain level of paging structure */
static inline uint32_t vtd_flpt_level_shift(uint32_t level)
{
    assert(level != 0);
    return VTD_PAGE_SHIFT_4K + (level - 1) * VTD_FL_LEVEL_BITS;
}

static inline uint64_t vtd_flpt_level_page_mask(uint32_t level)
{
    return ~((1ULL << vtd_flpt_level_shift(level)) - 1);
}

/* Given an iova and the level of paging structure, return the offset
 * of current level.
 */
static inline uint32_t vtd_iova_fl_level_offset(uint64_t iova, uint32_t level)
{
    return (iova >> vtd_flpt_level_shift(level)) &
            ((1ULL << VTD_FL_LEVEL_BITS) - 1);
}

/* Get the content of a flpte located in @base_addr[@index] */
static uint64_t vtd_get_flpte(dma_addr_t base_addr, uint32_t index)
{
    uint64_t flpte;

    assert(index < VTD_FL_PT_ENTRY_NR);

    if (dma_memory_read(&address_space_memory,
                        base_addr + index * sizeof(flpte), &flpte,
                        sizeof(flpte))) {
        flpte = (uint64_t)-1;
        return flpte;
    }
    flpte = le64_to_cpu(flpte);
    return flpte;
}

static inline bool vtd_flpte_present(uint64_t flpte)
{
    return !!(flpte & 0x1);
}

/* Whether the pte indicates the address of the page frame */
static inline bool vtd_is_last_flpte(uint64_t flpte, uint32_t level)
{
    return level == VTD_FL_PT_LEVEL || (flpte & VTD_FL_PT_PAGE_SIZE_MASK);
}

static inline uint64_t vtd_get_flpte_addr(uint64_t flpte, uint8_t aw)
{
    return flpte & VTD_FL_PT_BASE_ADDR_MASK(aw);
}

/* Return true if IOVA passes canonicality check, otherwise false. */
static inline bool vtd_iova_canonicality_check(uint64_t iova,
                                               uint32_t level)
{
    uint64_t va;
    uint8_t va_bits;

    if (level != 4 && level != 5) {
        error_report_once("%s: invalid level(%d) for first level paging.",
                          __func__, level);
        return false;

    }

    va_bits = (level * VTD_FL_LEVEL_BITS) + 12;
    va = (uint64_t)(((long)iova << (64 - va_bits)) >> (64 - va_bits));
    return va == iova;
}

static int vtd_flt_page_walk_level(dma_addr_t addr,
                                   uint64_t start, uint64_t end,
                                   uint32_t level, vtd_page_walk_info *info)
{
    bool read, write;
    uint32_t offset;
    uint64_t flpte;
    uint64_t iova = start;
    uint64_t iova_next;
    uint64_t subpage_size, subpage_mask;
    int ret = 0;
    IOMMUTLBEvent event;
    IOMMUNotifier *n;
    IOMMUMemoryRegion *iommu = (IOMMUMemoryRegion *)info->private;

    IOMMU_NOTIFIER_FOREACH(n, iommu) {
        if (n->iommu_idx == 0)
            break;
    }

    subpage_size = 1ULL << vtd_flpt_level_shift(level);
    subpage_mask = vtd_flpt_level_page_mask(level);
    event.entry.addr_mask = 0;

    while (iova < end) {
        iova_next = (iova & subpage_mask) + subpage_size;

        offset = vtd_iova_fl_level_offset(iova, level);
        flpte = vtd_get_flpte(addr, offset);

        if (flpte == (uint64_t)-1) {
            goto next;
        }

        if (vtd_flpte_present(flpte)) {
            read = true;
            write = flpte & VTD_FL_RW_MASK;
        } else {
            read = false;
            write = false;
        }

        if (!vtd_is_last_flpte(flpte, level) && vtd_flpte_present(flpte)) {
            addr = vtd_get_flpte_addr(flpte, info->aw);
            level--;
            vtd_flt_page_walk_level(addr, iova, MIN(iova_next, end), level, info);
        } else {
            event.type = IOMMU_NOTIFIER_UNMAP;
            event.entry.target_as = &address_space_memory;
            event.entry.iova = ((iova & subpage_mask) < n->start) ?
                               n->start : iova & subpage_mask;
            event.entry.perm = IOMMU_ACCESS_FLAG(read, write);;
            event.entry.addr_mask =
                    ((event.entry.iova + event.entry.addr_mask) > n->end) ?
                    (n->end - event.entry.iova) : ~subpage_mask;
            event.entry.translated_addr = vtd_get_flpte_addr(flpte, info->aw);
            if (info->hook_fn(&event, info->private) < 0) {
                return ret;
            }
        }

next:
        iova = iova_next;
    }

    return 0;
}

static int vtd_flt_page_walk(IntelIOMMUState *s, VTDContextEntry *ce,
                             uint64_t start, uint64_t end,
                             vtd_page_walk_info *info)
{
    VTDPASIDEntry pe;
    dma_addr_t addr;
    uint32_t level;
    int ret;

    ret = vtd_ce_get_rid2pasid_entry(s, ce, &pe);
    if (ret) {
        return ret;
    }

    addr = vtd_pe_get_flpt_base(&pe);
    level = vtd_pe_get_flpt_level(&pe);
    if (!vtd_is_flpm_supported(s, level)) {
        return VTD_FR_PASID_TABLE_ENTRY_INV;
    }

    if (!vtd_iova_canonicality_check(start, level) ||
        !vtd_iova_canonicality_check(end, level)) {
            return -VTD_FR_ADDR_NONCANONICAL;
    }

    return vtd_flt_page_walk_level(addr, start, end, level, info);
}

static int vtd_sync_flt_range(VTDAddressSpace *vtd_as,
                              VTDContextEntry *ce,
                              hwaddr addr, hwaddr size)
{
    IntelIOMMUState *s = vtd_as->iommu_state;
    vtd_page_walk_info info = {
        .hook_fn = vtd_sync_shadow_page_hook,
        .private = (void *)&vtd_as->iommu,
        .notify_unmap = true,
        .aw = s->aw_bits,
        .as = vtd_as,
        .domain_id = vtd_get_domain_id(s, ce),
    };

    return vtd_flt_page_walk(s, ce, addr, addr + size, &info);
}

/* Given the @iova, get relevant @flptep. @flpte_level will be the last level
 * of the translation, can be used for deciding the size of large page.
 */
static int vtd_iova_to_flpte(VTDPASIDEntry *pe, uint64_t iova, bool is_write,
                             uint64_t *flptep, uint32_t *flpte_level,
                             bool *reads, bool *writes, IntelIOMMUState *s)
{
    dma_addr_t addr = vtd_pe_get_flpt_base(pe);
    uint32_t level = vtd_pe_get_flpt_level(pe);
    uint32_t offset;
    uint64_t flpte;
    uint8_t aw_bits = s->aw_bits;

    if (!vtd_is_flpm_supported(s, level)) {
        return -VTD_FR_PASID_TABLE_ENTRY_INV;
    }

    if (!vtd_iova_canonicality_check(iova, level)) {
        return -VTD_FR_ADDR_NONCANONICAL;
    }

    while (true) {
        offset = vtd_iova_fl_level_offset(iova, level);
        flpte = vtd_get_flpte(addr, offset);
        if (flpte == (uint64_t)-1) {
            if (level == VTD_PE_GET_LEVEL(pe)) {
                /* Invalid programming of context-entry */
                return -VTD_FR_CONTEXT_ENTRY_INV;
            } else {
                return -VTD_FR_PAGING_ENTRY_INV;
            }
        }

        if (!vtd_flpte_present(flpte)) {
            *reads = false;
            *writes = false;
            return -VTD_FR_PAGING_ENTRY_INV;
        }

        *reads = true;
        *writes = (*writes) && (flpte & VTD_FL_RW_MASK);
        if (is_write && !(flpte & VTD_FL_RW_MASK)) {
            return -VTD_FR_WRITE;
        }

        if (vtd_is_last_flpte(flpte, level)) {
            *flptep = flpte;
            *flpte_level = level;
            return 0;
        }

        addr = vtd_get_flpte_addr(flpte, aw_bits);
        level--;
    }
}

static uint64_t vtd_get_piotlb_gfn(hwaddr addr, uint32_t level)
{
    return (addr & vtd_flpt_level_page_mask(level)) >> VTD_PAGE_SHIFT_4K;
}

static int vtd_get_piotlb_key(char *key, int key_size, uint64_t gfn,
                              uint32_t pasid, uint32_t level, uint16_t source_id)
{
    return snprintf(key, key_size, "rsv%010dsid%06dpasid%010dgfn%017lldlevel%01d",
                    0, source_id, pasid, (unsigned long long int)gfn, level);
}

static VTDIOTLBEntry *vtd_lookup_piotlb(IntelIOMMUState *s, uint32_t pasid,
                                        hwaddr addr, uint16_t source_id)
{
    VTDIOTLBEntry *entry;
    char key[64];
    int level;

    for (level = VTD_SL_PT_LEVEL; level < VTD_SL_PML4_LEVEL; level++) {
        vtd_get_piotlb_key(&key[0], 64, vtd_get_piotlb_gfn(addr, level),
                           pasid, level, source_id);
        entry = g_hash_table_lookup(s->p_iotlb, &key[0]);
        if (entry) {
            goto out;
        }
    }

out:
    return entry;
}

static void vtd_update_piotlb(IntelIOMMUState *s, uint32_t pasid,
                              uint16_t domain_id, hwaddr addr, uint64_t flpte,
                              uint8_t access_flags, uint32_t level,
                              uint16_t source_id)
{
    VTDIOTLBEntry *entry = g_malloc(sizeof(*entry));
    char *key = g_malloc(64);
    uint64_t gfn = vtd_get_piotlb_gfn(addr, level);

    if (g_hash_table_size(s->p_iotlb) >= VTD_PASID_IOTLB_MAX_SIZE) {
        vtd_reset_piotlb(s);
    }

    entry->gfn = gfn;
    entry->domain_id = domain_id;
    entry->pte = flpte;
    entry->pasid = pasid;
    entry->access_flags = access_flags;
    entry->mask = vtd_flpt_level_page_mask(level);
    vtd_get_piotlb_key(key, 64, gfn, pasid, level, source_id);
    g_hash_table_replace(s->p_iotlb, key, entry);
}

/* Map dev to pasid-entry then do a paging-structures walk to do a iommu
 * translation.
 *
 * Called from RCU critical section.
 *
 * @vtd_as: The untranslated address space
 * @bus_num: The bus number
 * @devfn: The devfn, which is the  combined of device and function number
 * @is_write: The access is a write operation
 * @entry: IOMMUTLBEntry that contain the addr to be translated and result
 *
 * Returns true if translation is successful, otherwise false.
 */
static bool vtd_do_iommu_fl_translate(VTDAddressSpace *vtd_as, PCIBus *bus,
                                      uint8_t devfn, hwaddr addr, bool is_write,
                                      IOMMUTLBEntry *entry)
{
    IntelIOMMUState *s = vtd_as->iommu_state;
    VTDContextEntry ce;
    VTDPASIDEntry pe;
    uint8_t bus_num = pci_bus_num(bus);
    uint64_t flpte, page_mask;
    uint32_t level;
    uint16_t source_id = vtd_make_source_id(bus_num, devfn);
    int ret;
    bool is_fpd_set = false;
    bool reads = true;
    bool writes = true;
    uint8_t access_flags;
    uint32_t pasid;
    VTDIOTLBEntry *piotlb_entry;

    /*
     * We have standalone memory region for interrupt addresses, we
     * should never receive translation requests in this region.
     */
    assert(!vtd_is_interrupt_addr(addr));

    ret = vtd_dev_to_context_entry(s, pci_bus_num(bus), devfn, &ce);
    if (ret) {
        error_report_once("%s: detected translation failure 1 "
                          "(dev=%02x:%02x:%02x, iova=0x%" PRIx64 ")",
                          __func__, pci_bus_num(bus),
                          VTD_PCI_SLOT(devfn),
                          VTD_PCI_FUNC(devfn),
                          addr);
        return false;
    }

    /* For emulated device IOVA translation, use RID2PASID. */
    pasid = vtd_dev_get_rid2pasid(s, pci_bus_num(bus), devfn);

    /* Try to fetch flpte form IOTLB */
    piotlb_entry = vtd_lookup_piotlb(s, pasid, addr, source_id);
    if (piotlb_entry) {
        trace_vtd_piotlb_page_hit(source_id, pasid, addr, piotlb_entry->pte,
                                  piotlb_entry->domain_id);
        flpte = piotlb_entry->pte;
        access_flags = piotlb_entry->access_flags;
        page_mask = piotlb_entry->mask;
        goto out;
    }

    vtd_iommu_lock(s);

    ret = vtd_ce_get_rid2pasid_entry(s, &ce, &pe);
    is_fpd_set = pe.val[0] & VTD_PASID_ENTRY_FPD;
    VTD_PE_GET_FPD_ERR(ret, is_fpd_set, s, source_id, addr, is_write);

    /*
     * We don't need to translate for pass-through context entries.
     * Also, let's ignore IOTLB caching as well for PT devices.
     */
    if (VTD_PE_GET_TYPE(&pe) == VTD_SM_PASID_ENTRY_PT) {
        entry->iova = addr & VTD_PAGE_MASK_4K;
        entry->translated_addr = entry->iova;
        entry->addr_mask = ~VTD_PAGE_MASK_4K;
        entry->perm = IOMMU_RW;
        vtd_iommu_unlock(s);
        return true;
    }

    ret = vtd_iova_to_flpte(&pe, addr, is_write, &flpte, &level,
                            &reads, &writes, s);
    VTD_PE_GET_FPD_ERR(ret, is_fpd_set, s, source_id, addr, is_write);

    page_mask = vtd_flpt_level_page_mask(level);
    access_flags = IOMMU_ACCESS_FLAG(reads, writes);

    vtd_update_piotlb(s, pasid, vtd_pe_get_domain_id(&pe), addr, flpte,
                      access_flags, level, source_id);
out:
    vtd_iommu_unlock(s);

    entry->iova = addr & page_mask;
    entry->translated_addr = vtd_get_flpte_addr(flpte, s->aw_bits) & page_mask;
    entry->addr_mask = ~page_mask;
    entry->perm = access_flags;
    return true;

error:
    vtd_iommu_unlock(s);
    entry->iova = 0;
    entry->translated_addr = 0;
    entry->addr_mask = 0;
    entry->perm = IOMMU_NONE;
    return false;
}

/* Map dev to context-entry then do a paging-structures walk to do a iommu
 * translation.
 *
 * Called from RCU critical section.
 *
 * @bus_num: The bus number
 * @devfn: The devfn, which is the  combined of device and function number
 * @is_write: The access is a write operation
 * @entry: IOMMUTLBEntry that contain the addr to be translated and result
 *
 * Returns true if translation is successful, otherwise false.
 */
static bool vtd_do_iommu_translate(VTDAddressSpace *vtd_as, PCIBus *bus,
                                   uint8_t devfn, hwaddr addr, bool is_write,
                                   IOMMUTLBEntry *entry)
{
    IntelIOMMUState *s = vtd_as->iommu_state;
    VTDContextEntry ce;
    uint8_t bus_num = pci_bus_num(bus);
    VTDContextCacheEntry *cc_entry;
    uint64_t slpte, page_mask;
    uint32_t level;
    uint16_t source_id = vtd_make_source_id(bus_num, devfn);
    int ret_fr;
    bool is_fpd_set = false;
    bool reads = true;
    bool writes = true;
    uint8_t access_flags;
    VTDIOTLBEntry *iotlb_entry;

    /*
     * We have standalone memory region for interrupt addresses, we
     * should never receive translation requests in this region.
     */
    assert(!vtd_is_interrupt_addr(addr));

    vtd_iommu_lock(s);

    cc_entry = &vtd_as->context_cache_entry;

    /* Try to fetch slpte form IOTLB */
    iotlb_entry = vtd_lookup_iotlb(s, source_id, addr);
    if (iotlb_entry) {
        trace_vtd_iotlb_page_hit(source_id, addr, iotlb_entry->pte,
                                 iotlb_entry->domain_id);
        slpte = iotlb_entry->pte;
        access_flags = iotlb_entry->access_flags;
        page_mask = iotlb_entry->mask;
        goto out;
    }

    /* Try to fetch context-entry from cache first */
    if (cc_entry->context_cache_gen == s->context_cache_gen) {
        trace_vtd_iotlb_cc_hit(bus_num, devfn, cc_entry->context_entry.hi,
                               cc_entry->context_entry.lo,
                               cc_entry->context_cache_gen);
        ce = cc_entry->context_entry;
        is_fpd_set = ce.lo & VTD_CONTEXT_ENTRY_FPD;
        if (!is_fpd_set && s->root_scalable) {
            ret_fr = vtd_ce_get_pasid_fpd(s, &ce, &is_fpd_set);
            VTD_PE_GET_FPD_ERR(ret_fr, is_fpd_set, s, source_id, addr, is_write);
        }
    } else {
        ret_fr = vtd_dev_to_context_entry(s, bus_num, devfn, &ce);
        is_fpd_set = ce.lo & VTD_CONTEXT_ENTRY_FPD;
        if (!ret_fr && !is_fpd_set && s->root_scalable) {
            ret_fr = vtd_ce_get_pasid_fpd(s, &ce, &is_fpd_set);
        }
        VTD_PE_GET_FPD_ERR(ret_fr, is_fpd_set, s, source_id, addr, is_write);
        /* Update context-cache */
        trace_vtd_iotlb_cc_update(bus_num, devfn, ce.hi, ce.lo,
                                  cc_entry->context_cache_gen,
                                  s->context_cache_gen);
        cc_entry->context_entry = ce;
        cc_entry->context_cache_gen = s->context_cache_gen;
    }

    /*
     * We don't need to translate for pass-through context entries.
     * Also, let's ignore IOTLB caching as well for PT devices.
     */
    if (vtd_ce_get_type(&ce) == VTD_CONTEXT_TT_PASS_THROUGH) {
        entry->iova = addr & VTD_PAGE_MASK_4K;
        entry->translated_addr = entry->iova;
        entry->addr_mask = ~VTD_PAGE_MASK_4K;
        entry->perm = IOMMU_RW;
        trace_vtd_translate_pt(source_id, entry->iova);

        /*
         * When this happens, it means firstly caching-mode is not
         * enabled, and this is the first passthrough translation for
         * the device. Let's enable the fast path for passthrough.
         *
         * When passthrough is disabled again for the device, we can
         * capture it via the context entry invalidation, then the
         * IOMMU region can be swapped back.
         */
        vtd_pt_enable_fast_path(s, source_id);
        vtd_iommu_unlock(s);
        return true;
    }

    ret_fr = vtd_iova_to_slpte(s, &ce, addr, is_write, &slpte, &level,
                               &reads, &writes, s->aw_bits);
    VTD_PE_GET_FPD_ERR(ret_fr, is_fpd_set, s, source_id, addr, is_write);

    page_mask = vtd_slpt_level_page_mask(level);
    access_flags = IOMMU_ACCESS_FLAG(reads, writes);
    vtd_update_iotlb(s, source_id, vtd_get_domain_id(s, &ce), addr, slpte,
                     access_flags, level);
out:
    vtd_iommu_unlock(s);
    entry->iova = addr & page_mask;
    entry->translated_addr = vtd_get_slpte_addr(slpte, s->aw_bits) & page_mask;
    entry->addr_mask = ~page_mask;
    entry->perm = access_flags;
    return true;

error:
    vtd_iommu_unlock(s);
    entry->iova = 0;
    entry->translated_addr = 0;
    entry->addr_mask = 0;
    entry->perm = IOMMU_NONE;
    return false;
}

static void vtd_root_table_setup(IntelIOMMUState *s)
{
    s->root = vtd_get_quad_raw(s, DMAR_RTADDR_REG);
    s->root &= VTD_RTADDR_ADDR_MASK(s->aw_bits);

    vtd_update_scalable_state(s);

    trace_vtd_reg_dmar_root(s->root, s->root_scalable);
}

static void vtd_iec_notify_all(IntelIOMMUState *s, bool global,
                               uint32_t index, uint32_t mask)
{
    x86_iommu_iec_notify_all(X86_IOMMU_DEVICE(s), global, index, mask);
}

static void vtd_interrupt_remap_table_setup(IntelIOMMUState *s)
{
    uint64_t value = 0;
    value = vtd_get_quad_raw(s, DMAR_IRTA_REG);
    s->intr_size = 1UL << ((value & VTD_IRTA_SIZE_MASK) + 1);
    s->intr_root = value & VTD_IRTA_ADDR_MASK(s->aw_bits);
    s->intr_eime = value & VTD_IRTA_EIME;

    /* Notify global invalidation */
    vtd_iec_notify_all(s, true, 0, 0);

    trace_vtd_reg_ir_root(s->intr_root, s->intr_size);
}

static void vtd_iommu_replay_all(IntelIOMMUState *s)
{
    VTDAddressSpace *vtd_as;

    QLIST_FOREACH(vtd_as, &s->vtd_as_with_notifiers, next) {
        vtd_sync_shadow_page_table(vtd_as);
    }
}

static void vtd_context_global_invalidate(IntelIOMMUState *s)
{
    VTDPASIDCacheInfo pc_info = { .error_happened = false, };

    trace_vtd_inv_desc_cc_global();

    /* Protects context cache */
    vtd_iommu_lock(s);
    s->context_cache_gen++;
    if (s->context_cache_gen == VTD_CONTEXT_CACHE_GEN_MAX) {
        vtd_reset_context_cache_locked(s);
    }
    vtd_iommu_unlock(s);
    vtd_address_space_refresh_all(s);
    /*
     * From VT-d spec 6.5.2.1, a global context entry invalidation
     * should be followed by a IOTLB global invalidation, so we should
     * be safe even without this. Hoewever, let's replay the region as
     * well to be safer, and go back here when we need finer tunes for
     * VT-d emulation codes.
     */
    vtd_iommu_replay_all(s);

    pc_info.type = VTD_PASID_CACHE_GLOBAL_INV;
    vtd_pasid_cache_sync(s, &pc_info);
}

static int vtd_dev_get_rid2pasid(IntelIOMMUState *s,
                                 uint8_t bus_num, uint8_t devfn)
{
    VTDContextEntry ce;
    int ret, pasid = 0;

    /*
     * Currently, ECAP.RPS bit is likely to be reported as "Clear".
     * And per VT-d 3.1 spec, it will use PASID #0 as RID2PASID when
     * RPS bit is reported as "Clear".
     */
    if (likely(!(s->ecap & VTD_ECAP_RPS))) {
        return 0;
    }

    /*
     * In future, to improve performance, could try to fetch context
     * entry from cache firstly.
     */
    ret = vtd_dev_to_context_entry(s, bus_num, devfn, &ce);
    if (ret) {
        return ret;
    }

    pasid = VTD_CE_GET_RID2PASID(&ce);
    return pasid;
}

static int vtd_hpasid_find_by_guest(IntelIOMMUState *s, uint32_t pasid);

/**
 * Caller should hold iommu_lock.
 */
static int vtd_bind_guest_pasid(VTDPASIDAddressSpace *vtd_pasid_as,
                                VTDPASIDEntry *pe, VTDPASIDOp op)
{
    IntelIOMMUState *s;
    VTDBus *vtd_bus;
    int devfn, pasid;
    VTDHostIOMMUContext *vtd_dev_icx;
    HostIOMMUContext *iommu_ctx;
    int ret = -1, rid2pasid, hpasid;

    assert(vtd_pasid_as);

    s = vtd_pasid_as->iommu_state;
    vtd_bus = vtd_pasid_as->vtd_bus;
    devfn = vtd_pasid_as->devfn;
    pasid = vtd_pasid_as->pasid;

    rid2pasid = vtd_dev_get_rid2pasid(s, pci_bus_num(vtd_bus->bus), devfn);
    if (unlikely(rid2pasid < 0)) {
        /* If unable to get rid2pasid form guest memroy, use PASID #0 */
        rid2pasid = 0;
    }

    if (pasid < VTD_HPASID_MIN && pasid != rid2pasid) {
        /*
         * If pasid < VTD_HPASID_MIN, this pasid is not allocated
         * from host. No need to pass down the changes on it to host.
         * TODO: when IOVA over FLPT is ready, this switch should be
         * refined.
         */
        return 0;
    }

    vtd_dev_icx = vtd_bus->dev_icx[devfn];
    if (!vtd_dev_icx) {
        /* means no need to go further, e.g. for emulated devices */
        return 0;
    }

    iommu_ctx = vtd_dev_icx->iommu_ctx;
    if (!iommu_ctx) {
        return -EINVAL;
    }

    /*
     * If a pasid # whose PGTT is switched from non-pt to pt, such case
     * means we need to unbind prior bind even if the op is BIND.
     */
    if (op == VTD_PASID_BIND &&
        vtd_pe_pt_enabled(pe) &&
        vtd_pasid_as->bound_to_host) {
        op = VTD_PASID_UNBIND;
    }

    hpasid = vtd_hpasid_find_by_guest(s, pasid);
    if (hpasid < 0) {
        error_report("%s, invalid bind, pasid: %d!\n", __func__, pasid);
    }

    switch (op) {
    case VTD_PASID_BIND:
    {
        struct iommu_gpasid_bind_data *g_bind_data;

        g_bind_data = g_malloc0(sizeof(*g_bind_data));

        g_bind_data->argsz = sizeof(*g_bind_data);
        g_bind_data->version = IOMMU_GPASID_BIND_VERSION_1;
        g_bind_data->format = IOMMU_PASID_FORMAT_INTEL_VTD;
        g_bind_data->flags = IOMMU_SVA_GPASID_VAL;
        g_bind_data->gpgd = vtd_pe_get_flpt_base(pe);
        g_bind_data->addr_width = vtd_pe_get_fl_aw(pe);
        g_bind_data->hpasid = hpasid;
        g_bind_data->gpasid = pasid;
        g_bind_data->flags |= IOMMU_SVA_GPASID_VAL;
        if (pasid == rid2pasid)
            g_bind_data->flags |= IOMMU_SVA_HPASID_DEF;
        g_bind_data->vendor.vtd.flags =
                             (VTD_SM_PASID_ENTRY_SRE_BIT(pe->val[2]) ?
                                            IOMMU_SVA_VTD_GPASID_SRE : 0)
                           | (VTD_SM_PASID_ENTRY_WPE_BIT(pe->val[2]) ?
                                            IOMMU_SVA_VTD_GPASID_WPE : 0)
                           | (VTD_SM_PASID_ENTRY_EAFE_BIT(pe->val[2]) ?
                                            IOMMU_SVA_VTD_GPASID_EAFE : 0)
                           | (VTD_SM_PASID_ENTRY_PCD_BIT(pe->val[1]) ?
                                            IOMMU_SVA_VTD_GPASID_PCD : 0)
                           | (VTD_SM_PASID_ENTRY_PWT_BIT(pe->val[1]) ?
                                            IOMMU_SVA_VTD_GPASID_PWT : 0)
                           | (VTD_SM_PASID_ENTRY_EMTE_BIT(pe->val[1]) ?
                                            IOMMU_SVA_VTD_GPASID_EMTE : 0)
                           | (VTD_SM_PASID_ENTRY_CD_BIT(pe->val[1]) ?
                                            IOMMU_SVA_VTD_GPASID_CD : 0);
        g_bind_data->vendor.vtd.pat = VTD_SM_PASID_ENTRY_PAT(pe->val[1]);
        g_bind_data->vendor.vtd.emt = VTD_SM_PASID_ENTRY_EMT(pe->val[1]);
        /* Only bind to host when guest pgtt!=pt */
        if (!vtd_pe_pt_enabled(pe)) {
            ret = host_iommu_ctx_bind_stage1_pgtbl(iommu_ctx, g_bind_data);
            if (ret == 0) {
                vtd_pasid_as->bound_to_host = true;
            }
        }
        g_free(g_bind_data);
        break;
    }
    case VTD_PASID_UNBIND:
    {
        struct iommu_gpasid_bind_data *g_unbind_data;

        g_unbind_data = g_malloc0(sizeof(*g_unbind_data));

        g_unbind_data->argsz = sizeof(*g_unbind_data);
        g_unbind_data->version = IOMMU_GPASID_BIND_VERSION_1;
        g_unbind_data->format = IOMMU_PASID_FORMAT_INTEL_VTD;
        g_unbind_data->hpasid = hpasid;
        g_unbind_data->gpasid = pasid;
        if (pasid == rid2pasid)
            g_unbind_data->flags |= IOMMU_SVA_HPASID_DEF;
        if (vtd_pasid_as->bound_to_host) {
            /* Only do unbind from host when it was bound once */
            ret = host_iommu_ctx_unbind_stage1_pgtbl(iommu_ctx, g_unbind_data);
            if (ret == 0) {
                vtd_pasid_as->bound_to_host = false;
            }
        }
        g_free(g_unbind_data);
        break;
    }
    default:
        error_report_once("Unknown VTDPASIDOp!!!\n");
        break;
    }


    return ret;
}

/* Do a context-cache device-selective invalidation.
 * @func_mask: FM field after shifting
 */
static void vtd_context_device_invalidate(IntelIOMMUState *s,
                                          uint16_t source_id,
                                          uint16_t func_mask)
{
    uint16_t mask;
    VTDBus *vtd_bus;
    VTDAddressSpace *vtd_as;
    uint8_t bus_n, devfn;
    uint16_t devfn_it;

    trace_vtd_inv_desc_cc_devices(source_id, func_mask);

    switch (func_mask & 3) {
    case 0:
        mask = 0;   /* No bits in the SID field masked */
        break;
    case 1:
        mask = 4;   /* Mask bit 2 in the SID field */
        break;
    case 2:
        mask = 6;   /* Mask bit 2:1 in the SID field */
        break;
    case 3:
        mask = 7;   /* Mask bit 2:0 in the SID field */
        break;
    default:
        g_assert_not_reached();
    }
    mask = ~mask;

    bus_n = VTD_SID_TO_BUS(source_id);
    vtd_bus = vtd_find_as_from_bus_num(s, bus_n);
    if (vtd_bus) {
        devfn = VTD_SID_TO_DEVFN(source_id);
        for (devfn_it = 0; devfn_it < PCI_DEVFN_MAX; ++devfn_it) {
            vtd_as = vtd_bus->dev_as[devfn_it];
            if (vtd_as && ((devfn_it & mask) == (devfn & mask))) {
                trace_vtd_inv_desc_cc_device(bus_n, VTD_PCI_SLOT(devfn_it),
                                             VTD_PCI_FUNC(devfn_it));
                vtd_iommu_lock(s);
                vtd_as->context_cache_entry.context_cache_gen = 0;
                vtd_iommu_unlock(s);
                /*
                 * Do switch address space when needed, in case if the
                 * device passthrough bit is switched.
                 */
                vtd_switch_address_space(vtd_as);
                /*
                 * So a device is moving out of (or moving into) a
                 * domain, resync the shadow page table.
                 * This won't bring bad even if we have no such
                 * notifier registered - the IOMMU notification
                 * framework will skip MAP notifications if that
                 * happened.
                 */
                vtd_sync_shadow_page_table(vtd_as);
                /*
                 * Per spec, context flush should also followed with PASID
                 * cache and iotlb flush. Regards to a device selective
                 * context cache invalidation:
                 * if (emaulted_device)
                 *    invalidate pasid cahce and pasid-based iotlb
                 * else if (assigned_device)
                 *    check if the device has been bound to any pasid
                 *    invoke pasid_unbind regards to each bound pasid
                 * Here, we have vtd_pasid_cache_devsi() to invalidate pasid
                 * caches, while for piotlb in QEMU, we don't have it yet, so
                 * no handling. For assigned device, host iommu driver would
                 * flush piotlb when a pasid unbind is pass down to it.
                 */
                 vtd_pasid_cache_devsi(s, vtd_bus, devfn_it);
            }
        }
    }
}

/* Context-cache invalidation
 * Returns the Context Actual Invalidation Granularity.
 * @val: the content of the CCMD_REG
 */
static uint64_t vtd_context_cache_invalidate(IntelIOMMUState *s, uint64_t val)
{
    uint64_t caig;
    uint64_t type = val & VTD_CCMD_CIRG_MASK;

    switch (type) {
    case VTD_CCMD_DOMAIN_INVL:
        /* Fall through */
    case VTD_CCMD_GLOBAL_INVL:
        caig = VTD_CCMD_GLOBAL_INVL_A;
        vtd_context_global_invalidate(s);
        break;

    case VTD_CCMD_DEVICE_INVL:
        caig = VTD_CCMD_DEVICE_INVL_A;
        vtd_context_device_invalidate(s, VTD_CCMD_SID(val), VTD_CCMD_FM(val));
        break;

    default:
        error_report_once("%s: invalid context: 0x%" PRIx64,
                          __func__, val);
        caig = 0;
    }
    return caig;
}

static void vtd_iotlb_global_invalidate(IntelIOMMUState *s)
{
    trace_vtd_inv_desc_iotlb_global();
    vtd_reset_iotlb(s);
    vtd_iommu_replay_all(s);
}

static void vtd_iotlb_domain_invalidate(IntelIOMMUState *s, uint16_t domain_id)
{
    VTDContextEntry ce;
    VTDAddressSpace *vtd_as;

    trace_vtd_inv_desc_iotlb_domain(domain_id);

    vtd_iommu_lock(s);
    g_hash_table_foreach_remove(s->iotlb, vtd_hash_remove_by_domain,
                                &domain_id);
    vtd_iommu_unlock(s);

    QLIST_FOREACH(vtd_as, &s->vtd_as_with_notifiers, next) {
        if (!vtd_dev_to_context_entry(s, pci_bus_num(vtd_as->bus),
                                      vtd_as->devfn, &ce) &&
            domain_id == vtd_get_domain_id(s, &ce)) {
            vtd_sync_shadow_page_table(vtd_as);
        }
    }
}

static void vtd_iotlb_page_invalidate_notify(IntelIOMMUState *s,
                                           uint16_t domain_id, hwaddr addr,
                                           uint8_t am)
{
    VTDAddressSpace *vtd_as;
    VTDContextEntry ce;
    int ret;
    hwaddr size = (1 << am) * VTD_PAGE_SIZE;

    QLIST_FOREACH(vtd_as, &(s->vtd_as_with_notifiers), next) {
        ret = vtd_dev_to_context_entry(s, pci_bus_num(vtd_as->bus),
                                       vtd_as->devfn, &ce);
        if (!ret && domain_id == vtd_get_domain_id(s, &ce)) {
            if (vtd_as_has_map_notifier(vtd_as)) {
                /*
                 * As long as we have MAP notifications registered in
                 * any of our IOMMU notifiers, we need to sync the
                 * shadow page table.
                 */
                vtd_sync_shadow_page_table_range(vtd_as, &ce, addr, size);
            } else {
                /*
                 * For UNMAP-only notifiers, we don't need to walk the
                 * page tables.  We just deliver the PSI down to
                 * invalidate caches.
                 */
                IOMMUTLBEvent event = {
                    .type = IOMMU_NOTIFIER_UNMAP,
                    .entry = {
                        .target_as = &address_space_memory,
                        .iova = addr,
                        .translated_addr = 0,
                        .addr_mask = size - 1,
                        .perm = IOMMU_NONE,
                    },
                };
                memory_region_notify_iommu(&vtd_as->iommu, 0, event);
            }
        }
    }
}

static void vtd_iotlb_page_invalidate(IntelIOMMUState *s, uint16_t domain_id,
                                      hwaddr addr, uint8_t am)
{
    VTDIOTLBPageInvInfo info;

    trace_vtd_inv_desc_iotlb_pages(domain_id, addr, am);

    assert(am <= VTD_MAMV);
    info.is_piotlb = false;
    info.domain_id = domain_id;
    info.addr = addr;
    info.mask = ~((1 << am) - 1);
    vtd_iommu_lock(s);
    g_hash_table_foreach_remove(s->iotlb, vtd_hash_remove_by_page, &info);
    vtd_iommu_unlock(s);
    vtd_iotlb_page_invalidate_notify(s, domain_id, addr, am);
}

/* Flush IOTLB
 * Returns the IOTLB Actual Invalidation Granularity.
 * @val: the content of the IOTLB_REG
 */
static uint64_t vtd_iotlb_flush(IntelIOMMUState *s, uint64_t val)
{
    uint64_t iaig;
    uint64_t type = val & VTD_TLB_FLUSH_GRANU_MASK;
    uint16_t domain_id;
    hwaddr addr;
    uint8_t am;

    switch (type) {
    case VTD_TLB_GLOBAL_FLUSH:
        iaig = VTD_TLB_GLOBAL_FLUSH_A;
        vtd_iotlb_global_invalidate(s);
        break;

    case VTD_TLB_DSI_FLUSH:
        domain_id = VTD_TLB_DID(val);
        iaig = VTD_TLB_DSI_FLUSH_A;
        vtd_iotlb_domain_invalidate(s, domain_id);
        break;

    case VTD_TLB_PSI_FLUSH:
        domain_id = VTD_TLB_DID(val);
        addr = vtd_get_quad_raw(s, DMAR_IVA_REG);
        am = VTD_IVA_AM(addr);
        addr = VTD_IVA_ADDR(addr);
        if (am > VTD_MAMV) {
            error_report_once("%s: address mask overflow: 0x%" PRIx64,
                              __func__, vtd_get_quad_raw(s, DMAR_IVA_REG));
            iaig = 0;
            break;
        }
        iaig = VTD_TLB_PSI_FLUSH_A;
        vtd_iotlb_page_invalidate(s, domain_id, addr, am);
        break;

    default:
        error_report_once("%s: invalid granularity: 0x%" PRIx64,
                          __func__, val);
        iaig = 0;
    }
    return iaig;
}

static void vtd_fetch_inv_desc(IntelIOMMUState *s);

static inline bool vtd_queued_inv_disable_check(IntelIOMMUState *s)
{
    return s->qi_enabled && (s->iq_tail == s->iq_head) &&
           (s->iq_last_desc_type == VTD_INV_DESC_WAIT);
}

static void vtd_handle_gcmd_qie(IntelIOMMUState *s, bool en)
{
    uint64_t iqa_val = vtd_get_quad_raw(s, DMAR_IQA_REG);

    trace_vtd_inv_qi_enable(en);

    if (en) {
        s->iq = iqa_val & VTD_IQA_IQA_MASK(s->aw_bits);
        /* 2^(x+8) entries */
        s->iq_size = 1UL << ((iqa_val & VTD_IQA_QS) + 8 - (s->iq_dw ? 1 : 0));
        s->qi_enabled = true;
        trace_vtd_inv_qi_setup(s->iq, s->iq_size);
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_QIES);

        if (s->iq_tail != 0) {
            /*
             * This is a spec violation but Windows guests are known to set up
             * Queued Invalidation this way so we allow the write and process
             * Invalidation Descriptors right away.
             */
            trace_vtd_warn_invalid_qi_tail(s->iq_tail);
            if (!(vtd_get_long_raw(s, DMAR_FSTS_REG) & VTD_FSTS_IQE)) {
                vtd_fetch_inv_desc(s);
            }
        }
    } else {
        if (vtd_queued_inv_disable_check(s)) {
            /* disable Queued Invalidation */
            vtd_set_quad_raw(s, DMAR_IQH_REG, 0);
            s->iq_head = 0;
            s->qi_enabled = false;
            /* Ok - report back to driver */
            vtd_set_clear_mask_long(s, DMAR_GSTS_REG, VTD_GSTS_QIES, 0);
        } else {
            error_report_once("%s: detected improper state when disable QI "
                              "(head=0x%x, tail=0x%x, last_type=%d)",
                              __func__,
                              s->iq_head, s->iq_tail, s->iq_last_desc_type);
        }
    }
}

/* Set Root Table Pointer */
static void vtd_handle_gcmd_srtp(IntelIOMMUState *s)
{
    vtd_root_table_setup(s);
    /* Ok - report back to driver */
    vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_RTPS);
    vtd_reset_caches(s);
    vtd_address_space_refresh_all(s);
}

/* Set Interrupt Remap Table Pointer */
static void vtd_handle_gcmd_sirtp(IntelIOMMUState *s)
{
    vtd_interrupt_remap_table_setup(s);
    /* Ok - report back to driver */
    vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_IRTPS);
}

/* Handle Translation Enable/Disable */
static void vtd_handle_gcmd_te(IntelIOMMUState *s, bool en)
{
    if (s->dmar_enabled == en) {
        return;
    }

    trace_vtd_dmar_enable(en);

    if (en) {
        s->dmar_enabled = true;
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_TES);
    } else {
        s->dmar_enabled = false;

        /* Clear the index of Fault Recording Register */
        s->next_frcd_reg = 0;
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, VTD_GSTS_TES, 0);
    }

    vtd_reset_caches(s);
    vtd_address_space_refresh_all(s);
}

/* Handle Interrupt Remap Enable/Disable */
static void vtd_handle_gcmd_ire(IntelIOMMUState *s, bool en)
{
    trace_vtd_ir_enable(en);

    if (en) {
        s->intr_enabled = true;
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_IRES);
    } else {
        s->intr_enabled = false;
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, VTD_GSTS_IRES, 0);
    }
}

/* Handle write to Global Command Register */
static void vtd_handle_gcmd_write(IntelIOMMUState *s)
{
    uint32_t status = vtd_get_long_raw(s, DMAR_GSTS_REG);
    uint32_t val = vtd_get_long_raw(s, DMAR_GCMD_REG);
    uint32_t changed = status ^ val;

    trace_vtd_reg_write_gcmd(status, val);
    if (changed & VTD_GCMD_TE) {
        /* Translation enable/disable */
        vtd_handle_gcmd_te(s, val & VTD_GCMD_TE);
    }
    if (val & VTD_GCMD_SRTP) {
        /* Set/update the root-table pointer */
        vtd_handle_gcmd_srtp(s);
    }
    if (changed & VTD_GCMD_QIE) {
        /* Queued Invalidation Enable */
        vtd_handle_gcmd_qie(s, val & VTD_GCMD_QIE);
    }
    if (val & VTD_GCMD_SIRTP) {
        /* Set/update the interrupt remapping root-table pointer */
        vtd_handle_gcmd_sirtp(s);
    }
    if (changed & VTD_GCMD_IRE) {
        /* Interrupt remap enable/disable */
        vtd_handle_gcmd_ire(s, val & VTD_GCMD_IRE);
    }
}

/* Handle write to Context Command Register */
static void vtd_handle_ccmd_write(IntelIOMMUState *s)
{
    uint64_t ret;
    uint64_t val = vtd_get_quad_raw(s, DMAR_CCMD_REG);

    /* Context-cache invalidation request */
    if (val & VTD_CCMD_ICC) {
        if (s->qi_enabled) {
            error_report_once("Queued Invalidation enabled, "
                              "should not use register-based invalidation");
            return;
        }
        ret = vtd_context_cache_invalidate(s, val);
        /* Invalidation completed. Change something to show */
        vtd_set_clear_mask_quad(s, DMAR_CCMD_REG, VTD_CCMD_ICC, 0ULL);
        ret = vtd_set_clear_mask_quad(s, DMAR_CCMD_REG, VTD_CCMD_CAIG_MASK,
                                      ret);
    }
}

/* Handle write to IOTLB Invalidation Register */
static void vtd_handle_iotlb_write(IntelIOMMUState *s)
{
    uint64_t ret;
    uint64_t val = vtd_get_quad_raw(s, DMAR_IOTLB_REG);

    /* IOTLB invalidation request */
    if (val & VTD_TLB_IVT) {
        if (s->qi_enabled) {
            error_report_once("Queued Invalidation enabled, "
                              "should not use register-based invalidation");
            return;
        }
        ret = vtd_iotlb_flush(s, val);
        /* Invalidation completed. Change something to show */
        vtd_set_clear_mask_quad(s, DMAR_IOTLB_REG, VTD_TLB_IVT, 0ULL);
        ret = vtd_set_clear_mask_quad(s, DMAR_IOTLB_REG,
                                      VTD_TLB_FLUSH_GRANU_MASK_A, ret);
    }
}

/* Fetch an Invalidation Descriptor from the Invalidation Queue */
static bool vtd_get_inv_desc(IntelIOMMUState *s,
                             VTDInvDesc *inv_desc)
{
    dma_addr_t base_addr = s->iq;
    uint32_t offset = s->iq_head;
    uint32_t dw = s->iq_dw ? 32 : 16;
    dma_addr_t addr = base_addr + offset * dw;

    if (dma_memory_read(&address_space_memory, addr, inv_desc, dw)) {
        error_report_once("Read INV DESC failed.");
        return false;
    }
    inv_desc->lo = le64_to_cpu(inv_desc->lo);
    inv_desc->hi = le64_to_cpu(inv_desc->hi);
    if (dw == 32) {
        inv_desc->val[2] = le64_to_cpu(inv_desc->val[2]);
        inv_desc->val[3] = le64_to_cpu(inv_desc->val[3]);
    }
    return true;
}

static bool vtd_process_wait_desc(IntelIOMMUState *s, VTDInvDesc *inv_desc)
{
    if ((inv_desc->hi & VTD_INV_DESC_WAIT_RSVD_HI) ||
        (inv_desc->lo & VTD_INV_DESC_WAIT_RSVD_LO)) {
        error_report_once("%s: invalid wait desc: hi=%"PRIx64", lo=%"PRIx64
                          " (reserved nonzero)", __func__, inv_desc->hi,
                          inv_desc->lo);
        return false;
    }
    if (inv_desc->lo & VTD_INV_DESC_WAIT_SW) {
        /* Status Write */
        uint32_t status_data = (uint32_t)(inv_desc->lo >>
                               VTD_INV_DESC_WAIT_DATA_SHIFT);

        assert(!(inv_desc->lo & VTD_INV_DESC_WAIT_IF));

        /* FIXME: need to be masked with HAW? */
        dma_addr_t status_addr = inv_desc->hi;
        trace_vtd_inv_desc_wait_sw(status_addr, status_data);
        status_data = cpu_to_le32(status_data);
        if (dma_memory_write(&address_space_memory, status_addr, &status_data,
                             sizeof(status_data))) {
            trace_vtd_inv_desc_wait_write_fail(inv_desc->hi, inv_desc->lo);
            return false;
        }
    } else if (inv_desc->lo & VTD_INV_DESC_WAIT_IF) {
        /* Interrupt flag */
        vtd_generate_completion_event(s);
    } else if (inv_desc->lo & VTD_INV_DESC_WAIT_FN) {
        /* Fence flag */
        VTD_DEBUG("%s this is a fence wait desc: hi: %llx, lo: %llx\n",
               __func__, (unsigned long long) inv_desc->hi, (unsigned long long) inv_desc->lo);
	/*
	 * TODO: per spec CH 7.10, such wait descriptor is to ensure
	 * all requests submitted to invalidation queue is processed
	 * before processing requests after this wait descriptor.
	 */
    } else {
        error_report_once("%s: invalid wait desc: hi=%"PRIx64", lo=%"PRIx64
                          " (unknown type)", __func__, inv_desc->hi,
                          inv_desc->lo);
        return false;
    }
    return true;
}

static bool vtd_process_context_cache_desc(IntelIOMMUState *s,
                                           VTDInvDesc *inv_desc)
{
    uint16_t sid, fmask;

    if ((inv_desc->lo & VTD_INV_DESC_CC_RSVD) || inv_desc->hi) {
        error_report_once("%s: invalid cc inv desc: hi=%"PRIx64", lo=%"PRIx64
                          " (reserved nonzero)", __func__, inv_desc->hi,
                          inv_desc->lo);
        return false;
    }
    switch (inv_desc->lo & VTD_INV_DESC_CC_G) {
    case VTD_INV_DESC_CC_DOMAIN:
        trace_vtd_inv_desc_cc_domain(
            (uint16_t)VTD_INV_DESC_CC_DID(inv_desc->lo));
        /* Fall through */
    case VTD_INV_DESC_CC_GLOBAL:
        vtd_context_global_invalidate(s);
        break;

    case VTD_INV_DESC_CC_DEVICE:
        sid = VTD_INV_DESC_CC_SID(inv_desc->lo);
        fmask = VTD_INV_DESC_CC_FM(inv_desc->lo);
        vtd_context_device_invalidate(s, sid, fmask);
        break;

    default:
        error_report_once("%s: invalid cc inv desc: hi=%"PRIx64", lo=%"PRIx64
                          " (invalid type)", __func__, inv_desc->hi,
                          inv_desc->lo);
        return false;
    }
    return true;
}

static bool vtd_process_iotlb_desc(IntelIOMMUState *s, VTDInvDesc *inv_desc)
{
    uint16_t domain_id;
    uint8_t am;
    hwaddr addr;

    if ((inv_desc->lo & VTD_INV_DESC_IOTLB_RSVD_LO) ||
        (inv_desc->hi & VTD_INV_DESC_IOTLB_RSVD_HI)) {
        error_report_once("%s: invalid iotlb inv desc: hi=0x%"PRIx64
                          ", lo=0x%"PRIx64" (reserved bits unzero)",
                          __func__, inv_desc->hi, inv_desc->lo);
        return false;
    }

    switch (inv_desc->lo & VTD_INV_DESC_IOTLB_G) {
    case VTD_INV_DESC_IOTLB_GLOBAL:
        vtd_iotlb_global_invalidate(s);
        break;

    case VTD_INV_DESC_IOTLB_DOMAIN:
        domain_id = VTD_INV_DESC_IOTLB_DID(inv_desc->lo);
        vtd_iotlb_domain_invalidate(s, domain_id);
        break;

    case VTD_INV_DESC_IOTLB_PAGE:
        domain_id = VTD_INV_DESC_IOTLB_DID(inv_desc->lo);
        addr = VTD_INV_DESC_IOTLB_ADDR(inv_desc->hi);
        am = VTD_INV_DESC_IOTLB_AM(inv_desc->hi);
        if (am > VTD_MAMV) {
            error_report_once("%s: invalid iotlb inv desc: hi=0x%"PRIx64
                              ", lo=0x%"PRIx64" (am=%u > VTD_MAMV=%u)",
                              __func__, inv_desc->hi, inv_desc->lo,
                              am, (unsigned)VTD_MAMV);
            return false;
        }
        vtd_iotlb_page_invalidate(s, domain_id, addr, am);
        break;

    default:
        error_report_once("%s: invalid iotlb inv desc: hi=0x%"PRIx64
                          ", lo=0x%"PRIx64" (type mismatch: 0x%llx)",
                          __func__, inv_desc->hi, inv_desc->lo,
                          inv_desc->lo & VTD_INV_DESC_IOTLB_G);
        return false;
    }
    return true;
}

static inline void vtd_init_pasid_key(uint32_t pasid,
                                     uint16_t sid,
                                     struct pasid_key *key)
{
    key->pasid = pasid;
    key->sid = sid;
}

static guint vtd_pasid_as_key_hash(gconstpointer v)
{
    struct pasid_key *key = (struct pasid_key *)v;
    uint32_t a, b, c;

    /* Jenkins hash */
    a = b = c = JHASH_INITVAL + sizeof(*key);
    a += key->sid;
    b += extract32(key->pasid, 0, 16);
    c += extract32(key->pasid, 16, 16);

    __jhash_mix(a, b, c);
    __jhash_final(a, b, c);

    return c;
}

static gboolean vtd_pasid_as_key_equal(gconstpointer v1, gconstpointer v2)
{
    const struct pasid_key *k1 = v1;
    const struct pasid_key *k2 = v2;

    return (k1->pasid == k2->pasid) && (k1->sid == k2->sid);
}

static inline int vtd_dev_get_pe_from_pasid(IntelIOMMUState *s,
                                            uint8_t bus_num,
                                            uint8_t devfn,
                                            uint32_t pasid,
                                            VTDPASIDEntry *pe)
{
    VTDContextEntry ce;
    int ret;
    dma_addr_t pasid_dir_base;

    if (!s->root_scalable) {
        return -VTD_FR_RTADDR_INV_TTM;
    }

    ret = vtd_dev_to_context_entry(s, bus_num, devfn, &ce);
    if (ret) {
        return ret;
    }

    pasid_dir_base = VTD_CE_GET_PASID_DIR_TABLE(&ce);
    ret = vtd_get_pe_from_pasid_table(s,
                                  pasid_dir_base, pasid, pe);

    return ret;
}

static bool vtd_pasid_entry_compare(VTDPASIDEntry *p1, VTDPASIDEntry *p2)
{
    return !memcmp(p1, p2, sizeof(*p1));
}

/**
 * This function fills in the pasid entry in &vtd_pasid_as. Caller
 * of this function should hold iommu_lock.
 */
static int vtd_fill_pe_in_cache(IntelIOMMUState *s,
                                VTDPASIDAddressSpace *vtd_pasid_as,
                                VTDPASIDEntry *pe)
{
    VTDPASIDCacheEntry *pc_entry = &vtd_pasid_as->pasid_cache_entry;

    if (vtd_pasid_entry_compare(pe, &pc_entry->pasid_entry)) {
        /* No need to go further as cached pasid entry is latest */
        return 0;
    }

    pc_entry->pasid_entry = *pe;
    return vtd_bind_guest_pasid(vtd_pasid_as, pe, VTD_PASID_BIND);
}

/**
 * This function is used to clear cached pasid entry in vtd_pasid_as
 * instances. Caller of this function should hold iommu_lock.
 */
static gboolean vtd_flush_pasid(gpointer key, gpointer value,
                                gpointer user_data)
{
    VTDPASIDCacheInfo *pc_info = user_data;
    VTDPASIDAddressSpace *vtd_pasid_as = value;
    IntelIOMMUState *s = vtd_pasid_as->iommu_state;
    VTDPASIDCacheEntry *pc_entry = &vtd_pasid_as->pasid_cache_entry;
    VTDBus *vtd_bus = vtd_pasid_as->vtd_bus;
    VTDPASIDEntry pe;
    VTDContextEntry ce;
    VTDHostIOMMUContext *vtd_dev_icx;
    VTDIOTLBPageInvInfo info;
    uint16_t did;
    uint32_t pasid;
    uint16_t devfn;
    int ret;

    did = vtd_pe_get_domain_id(&pc_entry->pasid_entry);
    pasid = vtd_pasid_as->pasid;
    devfn = vtd_pasid_as->devfn;
    vtd_dev_icx = vtd_bus->dev_icx[vtd_pasid_as->devfn];

    switch (pc_info->type) {
    case VTD_PASID_CACHE_FORCE_RESET:
        /* For passthr dev which gpasid has been bound, we should not force reset */
        if (pasid == 0 && vtd_pasid_as->bound_to_host &&
            s->root_scalable && likely(s->dmar_enabled) && vtd_dev_icx) {
            ret = vtd_dev_to_context_entry(s, pci_bus_num(vtd_bus->bus), devfn, &ce);
            if (!ret) {
                ret = vtd_ce_get_rid2pasid_entry(s, &ce, &pe);
                if (!ret && (VTD_PE_GET_TYPE(&pe) == VTD_SM_PASID_ENTRY_FLT)) {
                    return false;
                }
            }
        }
        goto remove;
    case VTD_PASID_CACHE_PASIDSI:
        if (pc_info->pasid != pasid) {
            return false;
        }
        /* Fall through */
    case VTD_PASID_CACHE_DOMSI:
        if (pc_info->domain_id != did) {
            return false;
        }
        /* Fall through */
    case VTD_PASID_CACHE_GLOBAL_INV:
        break;
    case VTD_PASID_CACHE_DEVSI:
        if (pc_info->vtd_bus != vtd_bus ||
            pc_info->devfn != devfn) {
            return false;
        }
        break;
    default:
        error_report("invalid pc_info->type");
        abort();
    }

    info.domain_id = did;
    info.pasid = pasid;
    /* For passthrough device, we don't need invalidate QEMU piotlb */
    if (s->root_scalable && likely(s->dmar_enabled) && !vtd_dev_icx)
        g_hash_table_foreach_remove(s->p_iotlb, vtd_hash_remove_by_pasid,
                                    &info);

    /*
     * pasid cache invalidation may indicate a present pasid
     * entry to present pasid entry modification. To cover such
     * case, vIOMMU emulator needs to fetch latest guest pasid
     * entry and check cached pasid entry, then update pasid
     * cache and send pasid bind/unbind to host properly.
     */
    ret = vtd_dev_get_pe_from_pasid(s, pci_bus_num(vtd_bus->bus),
                                    devfn, pasid, &pe);
    if (ret) {
        /*
         * No valid pasid entry in guest memory. e.g. pasid entry
         * was modified to be either all-zero or non-present. Either
         * case means existing pasid cache should be removed.
         */
        goto remove;
    }

    if (vtd_fill_pe_in_cache(s, vtd_pasid_as, &pe)) {
        pasid_cache_info_set_error(pc_info);
    }

    return false;

remove:
    if (vtd_bind_guest_pasid(vtd_pasid_as, &pe, VTD_PASID_UNBIND)) {
        pasid_cache_info_set_error(pc_info);
    }

    return true;
}

/**
 * This function finds or adds a VTDPASIDAddressSpace for a device
 * when it is bound to a pasid. Caller of this function should hold
 * iommu_lock.
 */
static VTDPASIDAddressSpace *vtd_add_find_pasid_as(IntelIOMMUState *s,
                                                   VTDBus *vtd_bus,
                                                   int devfn,
                                                   uint32_t pasid)
{
    struct pasid_key key;
    struct pasid_key *new_key;
    VTDPASIDAddressSpace *vtd_pasid_as;
    uint16_t sid;

    sid = vtd_make_source_id(pci_bus_num(vtd_bus->bus), devfn);
    vtd_init_pasid_key(pasid, sid, &key);
    vtd_pasid_as = g_hash_table_lookup(s->vtd_pasid_as, &key);

    if (!vtd_pasid_as) {
        new_key = g_malloc0(sizeof(*new_key));
        vtd_init_pasid_key(pasid, sid, new_key);
        /*
         * Initiate the vtd_pasid_as structure.
         *
         * This structure here is used to track the guest pasid
         * binding and also serves as pasid-cache mangement entry.
         *
         * TODO: in future, if wants to support the SVA-aware DMA
         *       emulation, the vtd_pasid_as should have include
         *       AddressSpace to support DMA emulation.
         */
        vtd_pasid_as = g_malloc0(sizeof(VTDPASIDAddressSpace));
        vtd_pasid_as->iommu_state = s;
        vtd_pasid_as->vtd_bus = vtd_bus;
        vtd_pasid_as->devfn = devfn;
        vtd_pasid_as->pasid = pasid;
        g_hash_table_insert(s->vtd_pasid_as, new_key, vtd_pasid_as);
    }
    return vtd_pasid_as;
}

/**
 * Caller of this function should hold iommu_lock.
 */
static void vtd_sm_pasid_table_walk_one(IntelIOMMUState *s,
                                        dma_addr_t pt_base,
                                        int start,
                                        int end,
                                        VTDPASIDCacheInfo *info)
{
    VTDPASIDEntry pe;
    int pasid = start;
    int pasid_next;
    VTDPASIDAddressSpace *vtd_pasid_as;

    while (pasid < end) {
        pasid_next = pasid + 1;

        if (!vtd_get_pe_in_pasid_leaf_table(s, pasid, pt_base, &pe)
            && vtd_pe_present(&pe)) {
            vtd_pasid_as = vtd_add_find_pasid_as(s,
                                       info->vtd_bus, info->devfn, pasid);
            if ((info->type == VTD_PASID_CACHE_DOMSI ||
                 info->type == VTD_PASID_CACHE_PASIDSI) &&
                !(info->domain_id == vtd_pe_get_domain_id(&pe))) {
                /*
                 * VTD_PASID_CACHE_DOMSI and VTD_PASID_CACHE_PASIDSI
                 * requires domain ID check. If domain Id check fail,
                 * go to next pasid.
                 */
                pasid = pasid_next;
                continue;
            }
            if (vtd_fill_pe_in_cache(s, vtd_pasid_as, &pe)) {
                pasid_cache_info_set_error(info);
            }
        }
        pasid = pasid_next;
    }
}

/*
 * Currently, VT-d scalable mode pasid table is a two level table,
 * this function aims to loop a range of PASIDs in a given pasid
 * table to identify the pasid config in guest.
 * Caller of this function should hold iommu_lock.
 */
static void vtd_sm_pasid_table_walk(IntelIOMMUState *s,
                                    dma_addr_t pdt_base,
                                    int start,
                                    int end,
                                    VTDPASIDCacheInfo *info)
{
    VTDPASIDDirEntry pdire;
    int pasid = start;
    int pasid_next;
    dma_addr_t pt_base;

    while (pasid < end) {
        pasid_next = ((end - pasid) > VTD_PASID_TBL_ENTRY_NUM) ?
                      (pasid + VTD_PASID_TBL_ENTRY_NUM) : end;
        if (!vtd_get_pdire_from_pdir_table(pdt_base, pasid, &pdire)
            && vtd_pdire_present(&pdire)) {
            pt_base = pdire.val & VTD_PASID_TABLE_BASE_ADDR_MASK;
            vtd_sm_pasid_table_walk_one(s, pt_base, pasid, pasid_next, info);
        }
        pasid = pasid_next;
    }
}

static void vtd_replay_pasid_bind_for_dev(IntelIOMMUState *s,
                                          int start, int end,
                                          VTDPASIDCacheInfo *info)
{
    VTDContextEntry ce;
    int bus_n, devfn;

    bus_n = pci_bus_num(info->vtd_bus->bus);
    devfn = info->devfn;

    if (!vtd_dev_to_context_entry(s, bus_n, devfn, &ce)) {
        uint32_t max_pasid;

        max_pasid = vtd_sm_ce_get_pdt_entry_num(&ce) * VTD_PASID_TBL_ENTRY_NUM;
        if (end > max_pasid) {
            end = max_pasid;
        }
        vtd_sm_pasid_table_walk(s,
                                VTD_CE_GET_PASID_DIR_TABLE(&ce),
                                start,
                                end,
                                info);
    }
}

/**
 * This function replay the guest pasid bindings to hots by
 * walking the guest PASID table. This ensures host will have
 * latest guest pasid bindings. Caller should hold iommu_lock.
 */
static void vtd_replay_guest_pasid_bindings(IntelIOMMUState *s,
                                            VTDPASIDCacheInfo *pc_info)
{
    VTDHostIOMMUContext *vtd_dev_icx;
    int start = 0, end = VTD_HPASID_MAX;
    VTDPASIDCacheInfo walk_info;

    switch (pc_info->type) {
    case VTD_PASID_CACHE_PASIDSI:
        start = pc_info->pasid;
        end = pc_info->pasid + 1;
        /*
         * PASID selective invalidation is within domain,
         * thus fall through.
         */
    case VTD_PASID_CACHE_DOMSI:
    case VTD_PASID_CACHE_GLOBAL_INV:
        /* loop all assigned devices */
        break;
    case VTD_PASID_CACHE_DEVSI:
        walk_info.vtd_bus = pc_info->vtd_bus;
        walk_info.devfn = pc_info->devfn;
        vtd_replay_pasid_bind_for_dev(s, start, end, &walk_info);
        return;
    case VTD_PASID_CACHE_FORCE_RESET:
        /* For force reset, no need to go further replay */
        return;
    default:
        error_report("invalid pc_info->type for replay");
        abort();
    }

    /*
     * In this replay, only needs to care about the devices which
     * are backed by host IOMMU. For such devices, their vtd_dev_icx
     * instances are in the s->vtd_dev_icx_list. For devices which
     * are not backed byhost IOMMU, it is not necessary to replay
     * the bindings since their cache could be re-created in the future
     * DMA address transaltion.
     */
    walk_info = *pc_info;
    QLIST_FOREACH(vtd_dev_icx, &s->vtd_dev_icx_list, next) {
        /* vtd_bus|devfn fields are not identical with pc_info */
        walk_info.vtd_bus = vtd_dev_icx->vtd_bus;
        walk_info.devfn = vtd_dev_icx->devfn;
        vtd_replay_pasid_bind_for_dev(s, start, end, &walk_info);
    }
    if (walk_info.error_happened) {
        pasid_cache_info_set_error(pc_info);
    }
}

/**
 * This function syncs the pasid bindings between guest and host.
 * It includes updating the pasid cache in vIOMMU and updating the
 * pasid bindings per guest's latest pasid entry presence.
 */
static void vtd_pasid_cache_sync(IntelIOMMUState *s,
                                 VTDPASIDCacheInfo *pc_info)
{
    if (!s->scalable_modern || !s->root_scalable)
        return;

    /*
     * Regards to a pasid cache invalidation, e.g. a PSI.
     * it could be either cases of below:
     * a) a present pasid entry moved to non-present
     * b) a present pasid entry to be a present entry
     * c) a non-present pasid entry moved to present
     *
     * Different invalidation granularity may affect different device
     * scope and pasid scope. But for each invalidation granularity,
     * it needs to do two steps to sync host and guest pasid binding.
     *
     * Here is the handling of a PSI:
     * 1) loop all the existing vtd_pasid_as instances to update them
     *    according to the latest guest pasid entry in pasid table.
     *    this will make sure affected existing vtd_pasid_as instances
     *    cached the latest pasid entries. Also, during the loop, the
     *    host should be notified if needed. e.g. pasid unbind or pasid
     *    update. Should be able to cover case a) and case b).
     *
     * 2) loop all devices to cover case c)
     *    - For devices which have HostIOMMUContext instances,
     *      we loop them and check if guest pasid entry exists. If yes,
     *      it is case c), we update the pasid cache and also notify
     *      host.
     *    - For devices which have no HostIOMMUContext, it is not
     *      necessary to create pasid cache at this phase since it
     *      could be created when vIOMMU does DMA address translation.
     *      This is not yet implemented since there is no emulated
     *      pasid-capable devices today. If we have such devices in
     *      future, the pasid cache shall be created there.
     * Other granularity follow the same steps, just with different scope
     *
     */

    vtd_iommu_lock(s);
    /* Step 1: loop all the exisitng vtd_pasid_as instances */
    g_hash_table_foreach_remove(s->vtd_pasid_as,
                                vtd_flush_pasid, pc_info);

    /*
     * Step 2: loop all the exisitng vtd_dev_icx instances.
     * Ideally, needs to loop all devices to find if there is any new
     * PASID binding regards to the PASID cache invalidation request.
     * But it is enough to loop the devices which are backed by host
     * IOMMU. For devices backed by vIOMMU (a.k.a emulated devices),
     * if new PASID happened on them, their vtd_pasid_as instance could
     * be created during future vIOMMU DMA translation.
     */
    vtd_replay_guest_pasid_bindings(s, pc_info);
    vtd_iommu_unlock(s);
}

static void vtd_pasid_cache_devsi(IntelIOMMUState *s,
                                  VTDBus *vtd_bus, uint16_t devfn)
{
    VTDPASIDCacheInfo pc_info = { .error_happened = false, };

    trace_vtd_pasid_cache_devsi(devfn);

    pc_info.type = VTD_PASID_CACHE_DEVSI;
    pc_info.vtd_bus = vtd_bus;
    pc_info.devfn = devfn;

    vtd_pasid_cache_sync(s, &pc_info);
}

/**
 * Caller of this function should hold iommu_lock
 */
static void vtd_pasid_cache_reset(IntelIOMMUState *s)
{
    VTDPASIDCacheInfo pc_info = { .error_happened = false, };

    trace_vtd_pasid_cache_reset();

    pc_info.type = VTD_PASID_CACHE_FORCE_RESET;

    /*
     * Reset pasid cache is a big hammer, so use
     * g_hash_table_foreach_remove which will free
     * the vtd_pasid_as instances. Also, as a big
     * hammer, use VTD_PASID_CACHE_FORCE_RESET to
     * ensure all the vtd_pasid_as instances are
     * dropped, meanwhile the change will be pass
     * to host if HostIOMMUContext is available.
     */
    g_hash_table_foreach_remove(s->vtd_pasid_as,
                                vtd_flush_pasid, &pc_info);
}

static bool vtd_process_pasid_desc(IntelIOMMUState *s,
                                   VTDInvDesc *inv_desc)
{
    VTDPASIDCacheInfo pc_info = { .error_happened = false, };
    uint16_t domain_id;
    uint32_t pasid;

    if ((inv_desc->val[0] & VTD_INV_DESC_PASIDC_RSVD_VAL0) ||
        (inv_desc->val[1] & VTD_INV_DESC_PASIDC_RSVD_VAL1) ||
        (inv_desc->val[2] & VTD_INV_DESC_PASIDC_RSVD_VAL2) ||
        (inv_desc->val[3] & VTD_INV_DESC_PASIDC_RSVD_VAL3)) {
        error_report_once("non-zero-field-in-pc_inv_desc hi: 0x%" PRIx64
                  " lo: 0x%" PRIx64, inv_desc->val[1], inv_desc->val[0]);
        return false;
    }

    domain_id = VTD_INV_DESC_PASIDC_DID(inv_desc->val[0]);
    pasid = VTD_INV_DESC_PASIDC_PASID(inv_desc->val[0]);

    switch (inv_desc->val[0] & VTD_INV_DESC_PASIDC_G) {
    case VTD_INV_DESC_PASIDC_DSI:
        trace_vtd_pasid_cache_dsi(domain_id);
        pc_info.type = VTD_PASID_CACHE_DOMSI;
        pc_info.domain_id = domain_id;
        break;

    case VTD_INV_DESC_PASIDC_PASID_SI:
        /* PASID selective implies a DID selective */
        pc_info.type = VTD_PASID_CACHE_PASIDSI;
        pc_info.domain_id = domain_id;
        pc_info.pasid = pasid;
        break;

    case VTD_INV_DESC_PASIDC_GLOBAL:
        trace_vtd_pasid_cache_gsi();
        pc_info.type = VTD_PASID_CACHE_GLOBAL_INV;
        break;

    default:
        error_report_once("invalid-inv-granu-in-pc_inv_desc hi: 0x%" PRIx64
                  " lo: 0x%" PRIx64, inv_desc->val[1], inv_desc->val[0]);
        return false;
    }

    vtd_pasid_cache_sync(s, &pc_info);
    return !pc_info.error_happened ? true : false;
}

/**
 * Caller of this function should hold iommu_lock.
 */
static void vtd_invalidate_piotlb(IntelIOMMUState *s,
                                  VTDBus *vtd_bus,
                                  int devfn,
                                  struct iommu_cache_invalidate_info *cache)
{
    VTDHostIOMMUContext *vtd_dev_icx;
    HostIOMMUContext *iommu_ctx;

    vtd_dev_icx = vtd_bus->dev_icx[devfn];
    if (!vtd_dev_icx) {
        goto out;
    }
    iommu_ctx = vtd_dev_icx->iommu_ctx;
    if (!iommu_ctx) {
        goto out;
    }
    if (host_iommu_ctx_flush_stage1_cache(iommu_ctx, cache)) {
        error_report("Cache flush failed");
    }
out:
    return;
}

/**
 * This function is a loop function for the s->vtd_pasid_as
 * list with VTDPIOTLBInvInfo as execution filter. It propagates
 * the piotlb invalidation to host. Caller of this function
 * should hold iommu_lock.
 */
static void vtd_flush_pasid_iotlb(gpointer key, gpointer value,
                                  gpointer user_data)
{
    VTDPIOTLBInvInfo *piotlb_info = user_data;
    VTDPASIDAddressSpace *vtd_pasid_as = value;
    VTDPASIDCacheEntry *pc_entry = &vtd_pasid_as->pasid_cache_entry;
    uint16_t did;
    int rid2pasid;
    int devfn = vtd_pasid_as->devfn;
    VTDBus *vtd_bus = vtd_pasid_as->vtd_bus;
    IntelIOMMUState *s = vtd_pasid_as->iommu_state;;

    rid2pasid = vtd_dev_get_rid2pasid(s, pci_bus_num(vtd_bus->bus), devfn);
    /*
     * For VT-d, by default, IOTLB type cache invalidation from guest
     * is PASID selective, by removing the FLAGS_PASID bit, host VT-d
     * driver should use default PASID.
     */
    if (piotlb_info->pasid != rid2pasid) {
        if (piotlb_info->cache_info->granularity == IOMMU_INV_GRANU_PASID) {
            piotlb_info->cache_info->granu.pasid_info.flags |=
                                        IOMMU_INV_PASID_FLAGS_PASID;
        } else {
            piotlb_info->cache_info->granu.addr_info.flags |=
                                        IOMMU_INV_ADDR_FLAGS_PASID;
        }
    }

    did = vtd_pe_get_domain_id(&pc_entry->pasid_entry);

    if ((piotlb_info->domain_id == did) &&
        (piotlb_info->pasid == vtd_pasid_as->pasid)) {
        vtd_invalidate_piotlb(vtd_pasid_as->iommu_state,
                              vtd_pasid_as->vtd_bus,
                              vtd_pasid_as->devfn,
                              piotlb_info->cache_info);
    }
}

static gboolean vtd_hash_remove_by_pasid(gpointer key, gpointer value,
                                         gpointer user_data)
{
    VTDIOTLBEntry *entry = (VTDIOTLBEntry *)value;
    VTDIOTLBPageInvInfo *info = (VTDIOTLBPageInvInfo *)user_data;

    return ((entry->domain_id == info->domain_id) &&
            (entry->pasid == info->pasid));
}

static void vtd_piotlb_pasid_invalidate(IntelIOMMUState *s,
                                        uint16_t domain_id,
                                        uint32_t pasid)
{
    VTDPIOTLBInvInfo piotlb_info;
    struct iommu_cache_invalidate_info *cache_info;
    VTDIOTLBPageInvInfo info;
    VTDAddressSpace *vtd_as;
    VTDContextEntry ce;
    VTDPASIDEntry pe;
    int ret;

    cache_info = g_malloc0(sizeof(*cache_info));

    cache_info->argsz = sizeof(*cache_info);
    cache_info->version = IOMMU_CACHE_INVALIDATE_INFO_VERSION_1;
    cache_info->cache = IOMMU_CACHE_INV_TYPE_IOTLB;
    cache_info->granularity = IOMMU_INV_GRANU_PASID;
    cache_info->granu.pasid_info.pasid = pasid;

    piotlb_info.domain_id = domain_id;
    piotlb_info.pasid = pasid;
    piotlb_info.cache_info = cache_info;

    info.domain_id = domain_id;
    info.pasid = pasid;

    vtd_iommu_lock(s);
    /*
     * Here loops all the vtd_pasid_as instances in s->vtd_pasid_as
     * to find out the affected devices since piotlb invalidation
     * should check pasid cache per architecture point of view.
     */
    g_hash_table_foreach(s->vtd_pasid_as,
                         vtd_flush_pasid_iotlb, &piotlb_info);
    g_hash_table_foreach_remove(s->p_iotlb, vtd_hash_remove_by_pasid,
                                &info);
    vtd_iommu_unlock(s);
    g_free(cache_info);

    QLIST_FOREACH(vtd_as, &(s->vtd_as_with_notifiers), next) {
        int rid2pasid;
        rid2pasid = vtd_dev_get_rid2pasid(s, pci_bus_num(vtd_as->bus), vtd_as->devfn);
        ret = vtd_dev_to_context_entry(s, pci_bus_num(vtd_as->bus), vtd_as->devfn, &ce);
        if (s->root_scalable && likely(s->dmar_enabled) &&
            domain_id == vtd_get_domain_id(s, &ce) &&
            !ret && pasid == rid2pasid) {
            ret = vtd_ce_get_rid2pasid_entry(s, &ce, &pe);
            if (!ret && VTD_PE_GET_TYPE(&pe) == VTD_SM_PASID_ENTRY_FLT) {
                ret = vtd_sync_flt_range(vtd_as, &ce, 0, UINT64_MAX);
            } else {
                ret = vtd_sync_shadow_page_table_range(vtd_as, &ce, 0, UINT64_MAX);
            }
        }
    }
}

static void vtd_piotlb_page_invalidate(IntelIOMMUState *s, uint16_t domain_id,
                                       uint32_t pasid, hwaddr addr, uint8_t am,
                                       bool ih)
{
    VTDPIOTLBInvInfo piotlb_info;
    struct iommu_cache_invalidate_info *cache_info;
    VTDIOTLBPageInvInfo info;
    VTDAddressSpace *vtd_as;
    VTDContextEntry ce;
    int ret;
    hwaddr size = (1 << am) * VTD_PAGE_SIZE;

    cache_info = g_malloc0(sizeof(*cache_info));

    cache_info->argsz = sizeof(*cache_info);
    cache_info->version = IOMMU_CACHE_INVALIDATE_INFO_VERSION_1;
    cache_info->cache = IOMMU_CACHE_INV_TYPE_IOTLB;
    cache_info->granularity = IOMMU_INV_GRANU_ADDR;
    cache_info->granu.addr_info.flags |= ih ? IOMMU_INV_ADDR_FLAGS_LEAF : 0;
    cache_info->granu.addr_info.pasid = pasid;
    cache_info->granu.addr_info.addr = addr;
    cache_info->granu.addr_info.granule_size = 1 << (12 + am);
    cache_info->granu.addr_info.nb_granules = 1;

    piotlb_info.domain_id = domain_id;
    piotlb_info.pasid = pasid;
    piotlb_info.cache_info = cache_info;

    info.is_piotlb = true;
    info.domain_id = domain_id;
    info.pasid = pasid;
    info.addr = addr;
    info.mask = ~((1 << am) - 1);

    vtd_iommu_lock(s);
    /*
     * Here loops all the vtd_pasid_as instances in s->vtd_pasid_as
     * to find out the affected devices since piotlb invalidation
     * should check pasid cache per architecture point of view.
     */
    g_hash_table_foreach(s->vtd_pasid_as,
                         vtd_flush_pasid_iotlb, &piotlb_info);
    g_hash_table_foreach_remove(s->p_iotlb, vtd_hash_remove_by_page, &info);
    vtd_iommu_unlock(s);
    g_free(cache_info);

    QLIST_FOREACH(vtd_as, &(s->vtd_as_with_notifiers), next) {
        ret = vtd_dev_to_context_entry(s, pci_bus_num(vtd_as->bus),
                                       vtd_as->devfn, &ce);
        if (!ret && domain_id == vtd_get_domain_id(s, &ce)) {
            if (vtd_as_has_map_notifier(vtd_as)) {
                error_report_once("%s: FLT does not do map, should not come"
                                  " here.\n", __func__);
            } else {
                IOMMUTLBEvent event;
                IOMMUTLBEntry entry = {
                    .target_as = &address_space_memory,
                    .iova = addr,
                    .translated_addr = 0,
                    .addr_mask = size - 1,
                    .perm = IOMMU_NONE,
                };

                event.type = IOMMU_NOTIFIER_UNMAP;
                event.entry = entry;
                memory_region_notify_iommu(&vtd_as->iommu, 0, event);
            }
        }
    }
}

static bool vtd_process_piotlb_desc(IntelIOMMUState *s,
                                    VTDInvDesc *inv_desc)
{
    uint16_t domain_id;
    uint32_t pasid;
    uint8_t am;
    hwaddr addr;

    if ((inv_desc->val[0] & VTD_INV_DESC_PIOTLB_RSVD_VAL0) ||
        (inv_desc->val[1] & VTD_INV_DESC_PIOTLB_RSVD_VAL1)) {
        error_report_once("non-zero-field-in-piotlb_inv_desc hi: 0x%" PRIx64
                  " lo: 0x%" PRIx64, inv_desc->val[1], inv_desc->val[0]);
        return false;
    }

    domain_id = VTD_INV_DESC_PIOTLB_DID(inv_desc->val[0]);
    pasid = VTD_INV_DESC_PIOTLB_PASID(inv_desc->val[0]);
    switch (inv_desc->val[0] & VTD_INV_DESC_IOTLB_G) {
    case VTD_INV_DESC_PIOTLB_ALL_IN_PASID:
        vtd_piotlb_pasid_invalidate(s, domain_id, pasid);
        break;

    case VTD_INV_DESC_PIOTLB_PSI_IN_PASID:
        am = VTD_INV_DESC_PIOTLB_AM(inv_desc->val[1]);
        addr = (hwaddr) VTD_INV_DESC_PIOTLB_ADDR(inv_desc->val[1]);
        vtd_piotlb_page_invalidate(s, domain_id, pasid, addr, am,
                                   VTD_INV_DESC_PIOTLB_IH(inv_desc->val[1]));
        break;

    default:
        error_report_once("Invalid granularity in P-IOTLB desc hi: 0x%" PRIx64
                  " lo: 0x%" PRIx64, inv_desc->val[1], inv_desc->val[0]);
        return false;
    }
    return true;
}

static bool vtd_process_inv_iec_desc(IntelIOMMUState *s,
                                     VTDInvDesc *inv_desc)
{
    trace_vtd_inv_desc_iec(inv_desc->iec.granularity,
                           inv_desc->iec.index,
                           inv_desc->iec.index_mask);

    vtd_iec_notify_all(s, !inv_desc->iec.granularity,
                       inv_desc->iec.index,
                       inv_desc->iec.index_mask);
    return true;
}

static bool vtd_process_device_piotlb_desc(IntelIOMMUState *s,
                                           VTDInvDesc *inv_desc)
{
    /*
     * no need to handle it for passthru device, for emulated
     * devices with device tlb, it may be required, but for now,
     * return is enough
     */
    return true;
}

static bool vtd_process_device_iotlb_desc(IntelIOMMUState *s,
                                          VTDInvDesc *inv_desc)
{
    VTDAddressSpace *vtd_dev_as;
    IOMMUTLBEvent event;
    struct VTDBus *vtd_bus;
    hwaddr addr;
    uint64_t sz;
    uint16_t sid;
    uint8_t devfn;
    bool size;
    uint8_t bus_num;

    addr = VTD_INV_DESC_DEVICE_IOTLB_ADDR(inv_desc->hi);
    sid = VTD_INV_DESC_DEVICE_IOTLB_SID(inv_desc->lo);
    devfn = sid & 0xff;
    bus_num = sid >> 8;
    size = VTD_INV_DESC_DEVICE_IOTLB_SIZE(inv_desc->hi);

    if ((inv_desc->lo & VTD_INV_DESC_DEVICE_IOTLB_RSVD_LO) ||
        (inv_desc->hi & VTD_INV_DESC_DEVICE_IOTLB_RSVD_HI)) {
        error_report_once("%s: invalid dev-iotlb inv desc: hi=%"PRIx64
                          ", lo=%"PRIx64" (reserved nonzero)", __func__,
                          inv_desc->hi, inv_desc->lo);
        return false;
    }

    vtd_bus = vtd_find_as_from_bus_num(s, bus_num);
    if (!vtd_bus) {
        goto done;
    }

    vtd_dev_as = vtd_bus->dev_as[devfn];
    if (!vtd_dev_as) {
        goto done;
    }

    /* According to ATS spec table 2.4:
     * S = 0, bits 15:12 = xxxx     range size: 4K
     * S = 1, bits 15:12 = xxx0     range size: 8K
     * S = 1, bits 15:12 = xx01     range size: 16K
     * S = 1, bits 15:12 = x011     range size: 32K
     * S = 1, bits 15:12 = 0111     range size: 64K
     * ...
     */
    if (size) {
        sz = (VTD_PAGE_SIZE * 2) << cto64(addr >> VTD_PAGE_SHIFT);
        addr &= ~(sz - 1);
    } else {
        sz = VTD_PAGE_SIZE;
    }

    event.type = IOMMU_NOTIFIER_DEVIOTLB_UNMAP;
    event.entry.target_as = &vtd_dev_as->as;
    event.entry.addr_mask = sz - 1;
    event.entry.iova = addr;
    event.entry.perm = IOMMU_NONE;
    event.entry.translated_addr = 0;
    memory_region_notify_iommu(&vtd_dev_as->iommu, 0, event);

done:
    return true;
}

static int vtd_dev_send_page_response(IntelIOMMUState *s, PCIBus *bus,
                                      int devfn,
                                      struct iommu_page_response *pg_resp)
{
    VTDHostIOMMUContext *vtd_dev_icx;
    HostIOMMUContext *iommu_ctx;
    VTDBus *vtd_bus;
    int ret = -EINVAL;

    vtd_bus = vtd_find_add_bus(s, bus);

    vtd_iommu_lock(s);
    vtd_dev_icx = vtd_bus->dev_icx[devfn];
    if (!vtd_dev_icx) {
        /* means no need to go further, e.g. for emulated devices */
        ret = 0;
        goto out_unlock;
    }
    iommu_ctx = vtd_dev_icx->iommu_ctx;
    if (!iommu_ctx) {
        ret = -EINVAL;
        goto out_unlock;
    }
    ret = host_iommu_ctx_page_response(iommu_ctx, pg_resp);
out_unlock:
    vtd_iommu_unlock(s);
    return ret;
}

static bool vtd_process_page_group_response(IntelIOMMUState *s,
                                            VTDInvDesc *inv_desc)
{
    struct iommu_page_response pg_resp;
    VTDPRQEntry *vtd_prq, *tmp;
    int hpasid;

    VTD_DEBUG("%s: page response: hi=0x%lx lo=0x%lx\n"
           , __func__, inv_desc->val[1], inv_desc->val[0]);
    /* Today only support page request with PASID, so the same with response */
    if (!inv_desc->resp.pasid_present) {
        return true;
    }

    vtd_iommu_lock(s);
    hpasid = vtd_hpasid_find_by_guest(s, inv_desc->resp.pasid);
    vtd_iommu_unlock(s);
    if (hpasid < 0) {
        VTD_DEBUG("%s, gpasid: %d not found!\n", __func__, inv_desc->resp.pasid);
        return true;
    }

    /*
     * REVISIT: private data from the guest is not sent back with
     * the page response in that host is tracking the private and
     * response is in order. Host IOMMU driver will match the private
     * data then respond back to the IOMMU hardware.
     */
    pg_resp.argsz = sizeof(pg_resp);
    pg_resp.version = IOMMU_PAGE_RESP_VERSION_1;
    pg_resp.code = inv_desc->resp.resp_code;
    pg_resp.grpid = inv_desc->resp.grpid;
    pg_resp.pasid = hpasid;
    pg_resp.flags = IOMMU_PAGE_RESP_PASID_VALID;
    VTD_DEBUG("%s, PASID %d pg_resp flags %x\n", __func__, pg_resp.pasid, pg_resp.flags);

    /*
     * YI: TODO: needs to do lpig and prg_index check in the prq
     * filtering. May just like what kernel does. How about pdp?
     * Should a pdp match also mean a hit? Will there be multiple
     *  prqs in the list with the same pasid?
     */
    qemu_mutex_lock(&s->prq_lock);
    QLIST_FOREACH_SAFE(vtd_prq, &s->vtd_prq_list, next, tmp) {
        if ((vtd_prq->prq.rid == inv_desc->resp.rid) &&
            (vtd_prq->prq.prg_index == inv_desc->resp.grpid)) {
            if ((vtd_prq->prq.pasid_present != inv_desc->resp.pasid_present) ||
                (inv_desc->resp.pasid_present &&
                                 vtd_prq->prq.pasid != inv_desc->resp.pasid)) {
                continue;
            }
            /*
             * Yi: if response failed, should return error to guest.
             * Que: shoud vIOMMU assume there will be multiple matched
             * prqs in the list? may be not as only prqs with lpig or
             * private data is added in the prq_list. Also one response
             * should response one prq. Only when the response succeed
             * , should the prq be freed in this list in case of guest
             * does retry.
             */
            if (!vtd_dev_send_page_response(s, vtd_prq->bus,
                                            vtd_prq->devfn, &pg_resp)) {
                QLIST_REMOVE(vtd_prq, next);
                g_free(vtd_prq);
            } else {
                error_report_once("%s: page response failed, resp_desc: "
                          "hi=%"PRIx64", lo=%"PRIx64, __func__,
                          inv_desc->val[1], inv_desc->val[0]);
            }
            break;
        }
    }
    qemu_mutex_unlock(&s->prq_lock);
    return true;
}

static bool vtd_process_inv_desc(IntelIOMMUState *s)
{
    VTDInvDesc inv_desc;
    uint8_t desc_type;

    trace_vtd_inv_qi_head(s->iq_head);
    if (!vtd_get_inv_desc(s, &inv_desc)) {
        s->iq_last_desc_type = VTD_INV_DESC_NONE;
        return false;
    }

    desc_type = inv_desc.lo & VTD_INV_DESC_TYPE;
    /* FIXME: should update at first or at last? */
    s->iq_last_desc_type = desc_type;

    switch (desc_type) {
    case VTD_INV_DESC_CC:
        trace_vtd_inv_desc("context-cache", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_context_cache_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_IOTLB:
        trace_vtd_inv_desc("iotlb", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_iotlb_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_PC:
        trace_vtd_inv_desc("pasid-cache", inv_desc.val[1], inv_desc.val[0]);
        if (!vtd_process_pasid_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_PIOTLB:
        trace_vtd_inv_desc("p-iotlb", inv_desc.val[1], inv_desc.val[0]);
        if (!vtd_process_piotlb_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_WAIT:
        trace_vtd_inv_desc("wait", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_wait_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_IEC:
        trace_vtd_inv_desc("iec", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_inv_iec_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_DEV_PIOTLB:
        trace_vtd_inv_desc("device-piotlb", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_device_piotlb_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_DEVICE:
        trace_vtd_inv_desc("device", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_device_iotlb_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_PAGE_GROUP_RESP:
        trace_vtd_inv_desc("GrpPgResp", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_page_group_response(s, &inv_desc)) {
            return false;
        }
        break;
    default:
        error_report_once("%s: invalid inv desc: hi=%"PRIx64", lo=%"PRIx64
                          " (unknown type)", __func__, inv_desc.hi,
                          inv_desc.lo);
        return false;
    }
    s->iq_head++;
    if (s->iq_head == s->iq_size) {
        s->iq_head = 0;
    }
    return true;
}

/* Try to fetch and process more Invalidation Descriptors */
static void vtd_fetch_inv_desc(IntelIOMMUState *s)
{
    int qi_shift;

    /* Refer to 10.4.23 of VT-d spec 3.0 */
    qi_shift = s->iq_dw ? VTD_IQH_QH_SHIFT_5 : VTD_IQH_QH_SHIFT_4;

    trace_vtd_inv_qi_fetch();

    if (s->iq_tail >= s->iq_size) {
        /* Detects an invalid Tail pointer */
        error_report_once("%s: detected invalid QI tail "
                          "(tail=0x%x, size=0x%x)",
                          __func__, s->iq_tail, s->iq_size);
        vtd_handle_inv_queue_error(s);
        return;
    }
    while (s->iq_head != s->iq_tail) {
        if (!vtd_process_inv_desc(s)) {
            /* Invalidation Queue Errors */
            vtd_handle_inv_queue_error(s);
            break;
        }
        /* Must update the IQH_REG in time */
        vtd_set_quad_raw(s, DMAR_IQH_REG,
                         (((uint64_t)(s->iq_head)) << qi_shift) &
                         VTD_IQH_QH_MASK);
    }
}

/* Handle write to Invalidation Queue Tail Register */
static void vtd_handle_iqt_write(IntelIOMMUState *s)
{
    uint64_t val = vtd_get_quad_raw(s, DMAR_IQT_REG);

    if (s->iq_dw && (val & VTD_IQT_QT_256_RSV_BIT)) {
        error_report_once("%s: RSV bit is set: val=0x%"PRIx64,
                          __func__, val);
        return;
    }
    s->iq_tail = VTD_IQT_QT(s->iq_dw, val);
    trace_vtd_inv_qi_tail(s->iq_tail);

    if (s->qi_enabled && !(vtd_get_long_raw(s, DMAR_FSTS_REG) & VTD_FSTS_IQE)) {
        /* Process Invalidation Queue here */
        vtd_fetch_inv_desc(s);
    }
}

static void vtd_handle_fsts_write(IntelIOMMUState *s)
{
    uint32_t fsts_reg = vtd_get_long_raw(s, DMAR_FSTS_REG);
    uint32_t fectl_reg = vtd_get_long_raw(s, DMAR_FECTL_REG);
    uint32_t status_fields = VTD_FSTS_PFO | VTD_FSTS_PPF | VTD_FSTS_IQE;

    if ((fectl_reg & VTD_FECTL_IP) && !(fsts_reg & status_fields)) {
        vtd_set_clear_mask_long(s, DMAR_FECTL_REG, VTD_FECTL_IP, 0);
        trace_vtd_fsts_clear_ip();
    }
    /* FIXME: when IQE is Clear, should we try to fetch some Invalidation
     * Descriptors if there are any when Queued Invalidation is enabled?
     */
}

static void vtd_handle_fectl_write(IntelIOMMUState *s)
{
    uint32_t fectl_reg;
    /* FIXME: when software clears the IM field, check the IP field. But do we
     * need to compare the old value and the new value to conclude that
     * software clears the IM field? Or just check if the IM field is zero?
     */
    fectl_reg = vtd_get_long_raw(s, DMAR_FECTL_REG);

    trace_vtd_reg_write_fectl(fectl_reg);

    if ((fectl_reg & VTD_FECTL_IP) && !(fectl_reg & VTD_FECTL_IM)) {
        vtd_generate_interrupt(s, DMAR_FEADDR_REG, DMAR_FEDATA_REG);
        vtd_set_clear_mask_long(s, DMAR_FECTL_REG, VTD_FECTL_IP, 0);
    }
}

static void vtd_handle_ics_write(IntelIOMMUState *s)
{
    uint32_t ics_reg = vtd_get_long_raw(s, DMAR_ICS_REG);
    uint32_t iectl_reg = vtd_get_long_raw(s, DMAR_IECTL_REG);

    if ((iectl_reg & VTD_IECTL_IP) && !(ics_reg & VTD_ICS_IWC)) {
        trace_vtd_reg_ics_clear_ip();
        vtd_set_clear_mask_long(s, DMAR_IECTL_REG, VTD_IECTL_IP, 0);
    }
}

static void vtd_handle_iectl_write(IntelIOMMUState *s)
{
    uint32_t iectl_reg;
    /* FIXME: when software clears the IM field, check the IP field. But do we
     * need to compare the old value and the new value to conclude that
     * software clears the IM field? Or just check if the IM field is zero?
     */
    iectl_reg = vtd_get_long_raw(s, DMAR_IECTL_REG);

    trace_vtd_reg_write_iectl(iectl_reg);

    if ((iectl_reg & VTD_IECTL_IP) && !(iectl_reg & VTD_IECTL_IM)) {
        vtd_generate_interrupt(s, DMAR_IEADDR_REG, DMAR_IEDATA_REG);
        vtd_set_clear_mask_long(s, DMAR_IECTL_REG, VTD_IECTL_IP, 0);
    }
}

/* Must be called with IOMMU lock held */
static VTDPASIDStoreEntry *vtd_pasid_find_by_idx(IntelIOMMUState *s,
                                                 uint32_t idx)
{
    VTDPASIDStoreEntry *entry;

    idx &= 0xfffff;
    entry = &s->vtd_pasid[idx >> 10][idx & 0x3ff];

    return entry->allocated ? entry : NULL;
}

/* Must be called with IOMMU lock held */
static int vtd_hpasid_find_by_guest(IntelIOMMUState *s, uint32_t pasid)
{
    VTDPASIDStoreEntry *entry;
    int ret;

    /* Identical g/h pasid */
    if (!s->non_identical_pasid) {
        return pasid;
    }

    entry = vtd_pasid_find_by_idx(s, pasid);
    if (entry) {
        ret = entry->hpasid;
    } else {
        ret = -ENODEV;
    }
    return ret;
}

/* Must be called with IOMMU lock held */
static int vtd_gpasid_find_by_host(IntelIOMMUState *s, uint32_t pasid)
{
    int j, k;

    /* Identical g/h pasid */
    if (!s->non_identical_pasid) {
        return pasid;
    }

    for (j = 0; j < 1024; j++) {
        for (k = 0; k < 1024; k++) {
            VTDPASIDStoreEntry *entry = &s->vtd_pasid[j][k];

            if (entry->allocated && entry->hpasid == pasid) {
                return entry->gpasid;
            }
        }
    }

    return -ENODEV;
}

/* Must be called with IOMMU lock held */
static void vtd_pasid_free_idx(IntelIOMMUState *s, uint32_t idx)
{
    VTDPASIDStoreEntry *entry;

    entry = vtd_pasid_find_by_idx(s, idx);
    if (entry) {
        memset(entry, 0x0, sizeof(*entry));
    }
}

/* Must be called with IOMMU lock held */
static VTDPASIDStoreEntry *vtd_pasid_alloc_idx(IntelIOMMUState *s)
{
    uint32_t idx;
    VTDPASIDStoreEntry *entry;

    idx = s->next_idx;
    while (vtd_pasid_find_by_idx(s, idx)) {
        if (idx == VTD_HPASID_MAX) {
            idx = VTD_HPASID_MIN;
        } else {
            idx = (idx + 1) & VTD_HPASID_MAX;
        }
        if (idx == s->next_idx) {
            return NULL;
        }
    }

    entry = &s->vtd_pasid[idx >> 10][idx & 0x3ff];
    entry->gpasid = idx;
    entry->allocated = true;
    s->next_idx = (idx + 1) & VTD_HPASID_MAX;
    return entry;
}

static int __vtd_alloc_host_pasid(IntelIOMMUState *s)
{
    struct ioasid_alloc_request req;
    int ret;

    req.argsz = sizeof(req);
    req.flags = 0;
    req.range.min = VTD_HPASID_MIN;
    req.range.max = VTD_HPASID_MAX;

    if (s->ioasid_fd < 0) {
        error_report("%s: No available allocation interface", __func__);
        return -1;
    }

    ret = ioctl(s->ioasid_fd, IOASID_REQUEST_ALLOC, &req);
    if (ret < 0) {
        error_report("%s: alloc failed %d", __func__, ret);
    }
    VTD_DEBUG("%s, allocated pasid: %d\n", __func__, ret);
    return ret;
}

static int vtd_request_pasid_alloc(IntelIOMMUState *s, uint32_t *pasid)
{
    int ret;
    VTDPASIDStoreEntry *entry = NULL;

    vtd_iommu_lock(s);
    ret = __vtd_alloc_host_pasid(s);
    if (ret < 0 || !s->non_identical_pasid) {
        if (ret > 0) {
            VTD_DEBUG("Allocated identical PASID g/h: %u/%u\n", ret, ret);
        }
        goto out;
    }

    entry = vtd_pasid_alloc_idx(s);
    if (entry) {
        entry->hpasid = ret;
        ret = entry->gpasid;
        VTD_DEBUG("Alloc PASID g/h: %u/%u\n", entry->gpasid, entry->hpasid);
    } else {
        ret = -ENOSPC;
    }
out:
    vtd_iommu_unlock(s);
    *pasid = ret;
    return (ret < 0) ? ret : 0;
}

static int __vtd_free_host_pasid(IntelIOMMUState *s, uint32_t pasid)
{
    int ret = -1;

    if (s->ioasid_fd < 0) {
        error_report("%s: No available allocation interface", __func__);
        return -1;
    }

    ret = ioctl(s->ioasid_fd, IOASID_REQUEST_FREE, &pasid);
    if (ret < 0) {
        error_report("%s: free failed (%m)", __func__);
    }

    return ret;
}

static int vtd_request_pasid_free(IntelIOMMUState *s, uint32_t pasid)
{
    int ret;
    VTDPASIDStoreEntry *entry = NULL;

    vtd_iommu_lock(s);

    /* Identical g/h pasid */
    if (!s->non_identical_pasid) {
        ret = __vtd_free_host_pasid(s, pasid);
        if (!ret) {
            VTD_DEBUG("Freed identical PASID g/h: %u/%u\n", pasid, pasid);
        }
        goto out;
    }

    entry = vtd_pasid_find_by_idx(s, pasid);
    if (!entry) {
        ret = -ENODEV;
        goto out;
    }
    ret = __vtd_free_host_pasid(s, entry->hpasid);
    if (!ret) {
        VTD_DEBUG("Free PASID g/h: %u/%u\n", pasid, entry->hpasid);
        vtd_pasid_free_idx(s, pasid);
    }
out:
    vtd_iommu_unlock(s);

    return ret;
}

/*
 * If IP is not set, set it then return.
 * If IP is already set, return.
 */
static void vtd_vcmd_set_ip(IntelIOMMUState *s)
{
    s->vcrsp = 1;
    vtd_set_quad_raw(s, DMAR_VCRSP_REG,
                     ((uint64_t) s->vcrsp));
}

static void vtd_vcmd_clear_ip(IntelIOMMUState *s)
{
    s->vcrsp &= (~((uint64_t)(0x1)));
    vtd_set_quad_raw(s, DMAR_VCRSP_REG,
                     ((uint64_t) s->vcrsp));
}

/* Handle write to Virtual Command Register */
static int vtd_handle_vcmd_write(IntelIOMMUState *s, uint64_t val)
{
    uint32_t pasid;
    int ret = -1;

    trace_vtd_reg_write_vcmd(s->vcrsp, val);

    if (!(s->vccap & VTD_VCCAP_PAS) ||
         (s->vcrsp & 1)) {
        return -1;
    }

    /*
     * Since vCPU should be blocked when the guest VMCD
     * write was trapped to here. Should be no other vCPUs
     * try to access VCMD if guest software is well written.
     * However, we still emulate the IP bit here in case of
     * bad guest software. Also align with the spec.
     */
    vtd_vcmd_set_ip(s);

    switch (val & VTD_VCMD_CMD_MASK) {
    case VTD_VCMD_ALLOC_PASID:
        ret = vtd_request_pasid_alloc(s, &pasid);
        if (ret) {
            s->vcrsp |= VTD_VCRSP_SC(VTD_VCMD_NO_AVAILABLE_PASID);
        } else {
            s->vcrsp |= VTD_VCRSP_RSLT(pasid);
        }
        break;

    case VTD_VCMD_FREE_PASID:
        pasid = VTD_VCMD_PASID_VALUE(val);
        ret = vtd_request_pasid_free(s, pasid);
        if (ret < 0) {
            s->vcrsp |= VTD_VCRSP_SC(VTD_VCMD_FREE_INVALID_PASID);
        }
        break;

    default:
        s->vcrsp |= VTD_VCRSP_SC(VTD_VCMD_UNDEFINED_CMD);
        error_report_once("Virtual Command: unsupported command!!!");
        break;
    }
    vtd_vcmd_clear_ip(s);
    return 0;
}

static void vtd_handle_prs_write(IntelIOMMUState *s)
{
    uint32_t prs_reg = vtd_get_long_raw(s, DMAR_PRS_REG);
    uint32_t pectl_reg = vtd_get_long_raw(s, DMAR_PECTL_REG);

    if ((pectl_reg & VTD_PECTL_IP) && !(prs_reg & VTD_PRS_PPR)) {
        vtd_set_clear_mask_long(s, DMAR_PECTL_REG, VTD_PECTL_IP, 0);
        VTD_DEBUG("pending completion interrupt condition serviced, "
                    "clear IP field of PECTL_REG\n");
    }
}

static void vtd_handle_pectl_write(IntelIOMMUState *s)
{
    uint32_t pectl_reg;
    /* FIXME: when software clears the IM field, check the IP field. But do
     * we need to compare the old value and the new value to conclude that
     * software clears the IM field? Or just check if the IM field is zero?
     */
    pectl_reg = vtd_get_long_raw(s, DMAR_PECTL_REG);
    if ((pectl_reg & VTD_PECTL_IP) && !(pectl_reg & VTD_PECTL_IM)) {
        vtd_generate_interrupt(s, DMAR_PEADDR_REG, DMAR_PEDATA_REG);
        vtd_set_clear_mask_long(s, DMAR_PECTL_REG, VTD_PECTL_IP, 0);
        VTD_DEBUG("IM field is cleared, generate "
                    "page request event interrupt\n");
    }
}

static void vtd_handle_pqh_write(IntelIOMMUState *s, uint64_t val)
{
    int head_nb, tail_nb;
    head_nb = (int) (val >> s->prq_entry_size_order);
    tail_nb = (int) (s->prq_tail >> s->prq_entry_size_order);
    /* Update prq_entry_count as consumer may have de-queue some entries */
    VTD_DEBUG("%s, head_n: %d, tail_nb: %d, old prq_head_nb: %lu\n", __func__, head_nb, tail_nb, (s->prq_head >> s->prq_entry_size_order));
    VTD_DEBUG("%s, s->prq_entry_count: %d - 1\n", __func__, s->prq_entry_count);
    qemu_mutex_lock(&s->prq_lock);
    s->prq_entry_count = (tail_nb - head_nb) & (s->prq_nb_entries - 1);
    VTD_DEBUG("%s, s->prq_entry_count: %d - 2\n", __func__, s->prq_entry_count);
    s->prq_head = val;
    qemu_mutex_unlock(&s->prq_lock);
}

static void vtd_handle_pqa_write(IntelIOMMUState *s, uint64_t val)
{
    qemu_mutex_lock(&s->prq_lock);
    s->pqa = val & VTD_PQA_ADDR_MASK(s->aw_bits);
    s->prq_head = 0;
    s->prq_tail = 0;
    /* per VT-d 3.0 spec, PRQ descriptor is 32 bytes */
    s->prq_entry_size_order = 5;
    s->prq_nb_entries = 1UL <<
             ((val & VTD_PQA_QS_MASK) + 12 - s->prq_entry_size_order);
    s->prq_qsize = 1ULL << ((val & VTD_PQA_QS_MASK) + 12);
    s->prq_entry_count = 0;
    qemu_mutex_unlock(&s->prq_lock);
}

static uint64_t vtd_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    IntelIOMMUState *s = opaque;
    uint64_t val;

    trace_vtd_reg_read(addr, size);

    if (addr + size > DMAR_REG_SIZE) {
        error_report_once("%s: MMIO over range: addr=0x%" PRIx64
                          " size=0x%x", __func__, addr, size);
        return (uint64_t)-1;
    }

    switch (addr) {
    /* Root Table Address Register, 64-bit */
    case DMAR_RTADDR_REG:
        val = vtd_get_quad_raw(s, DMAR_RTADDR_REG);
        if (size == 4) {
            val = val & ((1ULL << 32) - 1);
        }
        break;

    case DMAR_RTADDR_REG_HI:
        assert(size == 4);
        val = vtd_get_quad_raw(s, DMAR_RTADDR_REG) >> 32;
        break;

    /* Invalidation Queue Address Register, 64-bit */
    case DMAR_IQA_REG:
        val = s->iq | (vtd_get_quad(s, DMAR_IQA_REG) & VTD_IQA_QS);
        if (size == 4) {
            val = val & ((1ULL << 32) - 1);
        }
        break;

    case DMAR_IQA_REG_HI:
        assert(size == 4);
        val = s->iq >> 32;
        break;

    case DMAR_PQA_REG:
        val = s->pqa | (vtd_get_quad(s, DMAR_PQA_REG) & VTD_IQA_QS);
        if (size == 4) {
            val = val & ((1ULL << 32) - 1);
        }
        break;

    case DMAR_PQA_REG_HI:
        assert(size == 4);
        val = s->pqa >> 32;
        break;

    case DMAR_PQH_REG:
        val = s->prq_head;
        break;

    case DMAR_PQH_REG_HI:
        assert(size == 4);
        val = s->prq_head >> 32;
        break;

    case DMAR_PQT_REG:
        val = s->prq_tail;
        break;

    case DMAR_PQT_REG_HI:
        assert(size == 4);
        val = s->prq_tail >> 32;
        break;

    default:
        if (size == 4) {
            val = vtd_get_long(s, addr);
        } else {
            val = vtd_get_quad(s, addr);
        }
    }

    return val;
}

static void vtd_mem_write(void *opaque, hwaddr addr,
                          uint64_t val, unsigned size)
{
    IntelIOMMUState *s = opaque;

    trace_vtd_reg_write(addr, size, val);

    if (addr + size > DMAR_REG_SIZE) {
        error_report_once("%s: MMIO over range: addr=0x%" PRIx64
                          " size=0x%x", __func__, addr, size);
        return;
    }

    switch (addr) {
    /* Global Command Register, 32-bit */
    case DMAR_GCMD_REG:
        vtd_set_long(s, addr, val);
        vtd_handle_gcmd_write(s);
        break;

    /* Context Command Register, 64-bit */
    case DMAR_CCMD_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
            vtd_handle_ccmd_write(s);
        }
        break;

    case DMAR_CCMD_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_ccmd_write(s);
        break;

    /* IOTLB Invalidation Register, 64-bit */
    case DMAR_IOTLB_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
            vtd_handle_iotlb_write(s);
        }
        break;

    case DMAR_IOTLB_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_iotlb_write(s);
        break;

    /* Invalidate Address Register, 64-bit */
    case DMAR_IVA_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        break;

    case DMAR_IVA_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Fault Status Register, 32-bit */
    case DMAR_FSTS_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_fsts_write(s);
        break;

    /* Fault Event Control Register, 32-bit */
    case DMAR_FECTL_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_fectl_write(s);
        break;

    /* Fault Event Data Register, 32-bit */
    case DMAR_FEDATA_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Fault Event Address Register, 32-bit */
    case DMAR_FEADDR_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            /*
             * While the register is 32-bit only, some guests (Xen...) write to
             * it with 64-bit.
             */
            vtd_set_quad(s, addr, val);
        }
        break;

    /* Fault Event Upper Address Register, 32-bit */
    case DMAR_FEUADDR_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Protected Memory Enable Register, 32-bit */
    case DMAR_PMEN_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Root Table Address Register, 64-bit */
    case DMAR_RTADDR_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        break;

    case DMAR_RTADDR_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Invalidation Queue Tail Register, 64-bit */
    case DMAR_IQT_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        vtd_handle_iqt_write(s);
        break;

    case DMAR_IQT_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        /* 19:63 of IQT_REG is RsvdZ, do nothing here */
        break;

    /* Invalidation Queue Address Register, 64-bit */
    case DMAR_IQA_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        if (s->ecap & VTD_ECAP_SMTS &&
            val & VTD_IQA_DW_MASK) {
            s->iq_dw = true;
        } else {
            s->iq_dw = false;
        }
        break;

    case DMAR_IQA_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Invalidation Completion Status Register, 32-bit */
    case DMAR_ICS_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_ics_write(s);
        break;

    /* Invalidation Event Control Register, 32-bit */
    case DMAR_IECTL_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_iectl_write(s);
        break;

    /* Invalidation Event Data Register, 32-bit */
    case DMAR_IEDATA_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Invalidation Event Address Register, 32-bit */
    case DMAR_IEADDR_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Invalidation Event Upper Address Register, 32-bit */
    case DMAR_IEUADDR_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Page Request Queue Head Register, 64-bit */
    case DMAR_PQH_REG:
        VTD_DEBUG("%s, DMAR_PQH_REG write addr 0x%lx, size: %d, val: 0x%lx\n",
                __func__, addr, size, val);
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        vtd_handle_pqh_write(s, val);
        break;

    case DMAR_PQH_REG_HI:
        VTD_DEBUG("%s, DMAR_PQH_REG_HI write addr 0x%lx, size: %d, val: 0x%lx\n",
                __func__, addr, size, val);
        assert(size == 4);
        vtd_set_long(s, addr, val);
        /* 19:63 of PQH_REG is RsvdZ, do nothing here */
        break;

    /* Page Request Queue Tail Register, 64-bit */
    case DMAR_PQT_REG:
        VTD_DEBUG("%s, DMAR_PQT_REG write addr 0x%lx, size: %d, val: 0x%lx\n",
                __func__, addr, size, val);
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        s->prq_tail = val;
        break;

    case DMAR_PQT_REG_HI:
        VTD_DEBUG("%s, DMAR_PQT_REG_HI write addr 0x%lx, size: %d, val: 0x%lx\n",
                __func__, addr, size, val);
        assert(size == 4);
        vtd_set_long(s, addr, val);
        /* 19:63 of PQT_REG is RsvdZ, do nothing here */
        break;

    /* Page Request Queue Address Register, 64-bit */
    case DMAR_PQA_REG:
        VTD_DEBUG("%s, DMAR_PQA_REG write addr 0x%lx, size: %d, val: 0x%lx\n",
                __func__, addr, size, val);
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        vtd_handle_pqa_write(s, val);
        break;

    case DMAR_PQA_REG_HI:
        VTD_DEBUG("%s, DMAR_PQA_REG_HI write addr 0x%lx, size: %d, val: 0x%lx\n",
                __func__, addr, size, val);
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Page Request Status Register, 32-bit */
    case DMAR_PRS_REG:
        VTD_DEBUG("%s, DMAR_PRS_REG write addr 0x%lx, size: %d, val: 0x%lx\n",
                __func__, addr, size, val);
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_prs_write(s);
        break;

    /* Page Request Event Control Register, 32-bit */
    case DMAR_PECTL_REG:
        VTD_DEBUG("%s, DMAR_PECTL_REG write addr 0x%lx, size: %d, val: 0x%lx\n",
                __func__, addr, size, val);
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_pectl_write(s);
        break;

    /* Page Request Event Data Register, 32-bit */
    case DMAR_PEDATA_REG:
        VTD_DEBUG("%s, DMAR_PEDATA_REG write addr 0x%lx, size: %d, val: 0x%lx\n",
                __func__, addr, size, val);
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Page Request Event Address Register, 32-bit */
    case DMAR_PEADDR_REG:
        VTD_DEBUG("%s, DMAR_PEADDR_REG write addr 0x%lx, size: %d, val: 0x%lx\n",
                __func__, addr, size, val);
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Page Request Event Upper Address Register, 32-bit */
    case DMAR_PEUADDR_REG:
        VTD_DEBUG("%s, DMAR_PEUADDR_REG write addr 0x%lx, size: %d, val: 0x%lx\n",
                __func__, addr, size, val);
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Fault Recording Registers, 128-bit */
    case DMAR_FRCD_REG_0_0:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        break;

    case DMAR_FRCD_REG_0_1:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    case DMAR_FRCD_REG_0_2:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
            /* May clear bit 127 (Fault), update PPF */
            vtd_update_fsts_ppf(s);
        }
        break;

    case DMAR_FRCD_REG_0_3:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        /* May clear bit 127 (Fault), update PPF */
        vtd_update_fsts_ppf(s);
        break;

    case DMAR_IRTA_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        break;

    case DMAR_IRTA_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    case DMAR_VCMD_REG:
        if (!vtd_handle_vcmd_write(s, val)) {
            if (size == 4) {
                vtd_set_long(s, addr, val);
            } else {
                vtd_set_quad(s, addr, val);
            }
        }
        break;

    case DMAR_VCMD_REG_HI:
        assert(size == 4);
        if (!vtd_handle_vcmd_write(s, val)) {
            vtd_set_long(s, addr, val);
        }
        break;

    default:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
    }
}

static IOMMUTLBEntry vtd_iommu_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                         IOMMUAccessFlags flag, int iommu_idx)
{
    VTDAddressSpace *vtd_as = container_of(iommu, VTDAddressSpace, iommu);
    IntelIOMMUState *s = vtd_as->iommu_state;
    IOMMUTLBEntry iotlb = {
        /* We'll fill in the rest later. */
        .target_as = &address_space_memory,
    };
    bool success;
    VTDContextEntry ce;
    VTDPASIDEntry pe;
    int ret = 0;

    if (likely(s->dmar_enabled)) {
        if (s->root_scalable) {
            ret = vtd_dev_to_context_entry(s, pci_bus_num(vtd_as->bus),
                                           vtd_as->devfn, &ce);
            ret = vtd_ce_get_rid2pasid_entry(s, &ce, &pe);
            if (ret) {
                error_report_once("%s: detected translation failure 1 "
                                  "(dev=%02x:%02x:%02x, iova=0x%" PRIx64 ")",
                                  __func__, pci_bus_num(vtd_as->bus),
                                  VTD_PCI_SLOT(vtd_as->devfn),
                                  VTD_PCI_FUNC(vtd_as->devfn),
                                  addr);
                return iotlb;
            }
            if (VTD_PE_GET_TYPE(&pe) == VTD_SM_PASID_ENTRY_FLT) {
                success = vtd_do_iommu_fl_translate(vtd_as, vtd_as->bus, vtd_as->devfn,
                                                    addr, flag & IOMMU_WO, &iotlb);
            } else {
                success = vtd_do_iommu_translate(vtd_as, vtd_as->bus, vtd_as->devfn,
                                                 addr, flag & IOMMU_WO, &iotlb);
            }
        } else {
            success = vtd_do_iommu_translate(vtd_as, vtd_as->bus, vtd_as->devfn,
                                             addr, flag & IOMMU_WO, &iotlb);
        }
    } else {
        /* DMAR disabled, passthrough, use 4k-page*/
        iotlb.iova = addr & VTD_PAGE_MASK_4K;
        iotlb.translated_addr = addr & VTD_PAGE_MASK_4K;
        iotlb.addr_mask = ~VTD_PAGE_MASK_4K;
        iotlb.perm = IOMMU_RW;
        success = true;
    }

    if (likely(success)) {
        trace_vtd_dmar_translate(pci_bus_num(vtd_as->bus),
                                 VTD_PCI_SLOT(vtd_as->devfn),
                                 VTD_PCI_FUNC(vtd_as->devfn),
                                 iotlb.iova, iotlb.translated_addr,
                                 iotlb.addr_mask);
    } else {
        error_report_once("%s: detected translation failure "
                          "(dev=%02x:%02x:%02x, iova=0x%" PRIx64 ")",
                          __func__, pci_bus_num(vtd_as->bus),
                          VTD_PCI_SLOT(vtd_as->devfn),
                          VTD_PCI_FUNC(vtd_as->devfn),
                          addr);
    }

    return iotlb;
}

static int vtd_iommu_notify_flag_changed(IOMMUMemoryRegion *iommu,
                                         IOMMUNotifierFlag old,
                                         IOMMUNotifierFlag new,
                                         Error **errp)
{
    VTDAddressSpace *vtd_as = container_of(iommu, VTDAddressSpace, iommu);
    IntelIOMMUState *s = vtd_as->iommu_state;

    /* Update per-address-space notifier flags */
    vtd_as->notifier_flags = new;

    if (old == IOMMU_NOTIFIER_NONE) {
        QLIST_INSERT_HEAD(&s->vtd_as_with_notifiers, vtd_as, next);
    } else if (new == IOMMU_NOTIFIER_NONE) {
        QLIST_REMOVE(vtd_as, next);
    }
    return 0;
}

static void vtd_replay_pasid_allocation(IntelIOMMUState *s)
{
    int j, k;

    for (j = 0; j < 1024; j++) {
        for (k = 0; k < 1024; k++) {
            VTDPASIDStoreEntry *entry = &s->vtd_pasid[j][k];
            int ret;

            if (!j && !k) {
                /* no need to do reallocation for PASID#0 */
                continue;
            }
            if (entry->allocated) {
                ret = __vtd_alloc_host_pasid(s);
                if (ret < 0) {
                    error_report_once("%s: gpasid: %u failed to get"
                          " correspond hpasid", __func__, entry->gpasid);
                    continue;
                }
                entry->hpasid = ret;
                printf("%s, alloc gpasid: %u, hpasid: %d, j/k (%d/%d)\n", __func__, entry->gpasid, ret, j, k);
            }
        }
    }
}

static void vtd_migration_replay_pasid(IntelIOMMUState *s)
{
    VTDPASIDCacheInfo pc_info = { .error_happened = false,
                                  .type = VTD_PASID_CACHE_GLOBAL_INV };
    vtd_iommu_lock(s);
    /*
     * Replay pasid related stuffs:
     * a) replay pasid allocation according to the allocated
     *     per-vm pasids (gpasid);
     * b) replay pasid bindings;
     */
    vtd_replay_pasid_allocation(s);
    vtd_replay_guest_pasid_bindings(s, &pc_info);
    vtd_iommu_unlock(s);
}

static int vtd_post_load(void *opaque, int version_id)
{
    IntelIOMMUState *iommu = opaque;

    /*
     * We don't need to migrate the root_scalable because we can
     * simply do the calculation after the loading is complete.  We
     * can actually do similar things with root, dmar_enabled, etc.
     * however since we've had them already so we'd better keep them
     * for compatibility of migration.
     */
    vtd_update_scalable_state(iommu);

    /*
     * Memory regions are dynamically turned on/off depending on
     * context entry configurations from the guest. After migration,
     * we need to make sure the memory regions are still correct.
     */
    vtd_switch_address_space_all(iommu);

    if (iommu->scalable_modern && iommu->root_scalable) {
        printf("Replay pasid bindings after migration\n");
        vtd_migration_replay_pasid(iommu);
    } else {
        printf("Replay DMA mappings after migration\n");
        vtd_iommu_replay_all(iommu);
    }

    return 0;
}

static int vtd_save_setup(QEMUFile *f, void *opaque)
{
    int ret;

    qemu_put_be64(f, 0);

    ret = qemu_file_get_error(f);
    if (ret) {
        return ret;
    }
    return 0;
}

static void vtd_save_cleanup(void *opaque)
{
}

static void vtd_save_pending(QEMUFile *f, void *opaque,
                              uint64_t threshold_size,
                              uint64_t *res_precopy_only,
                              uint64_t *res_compatible,
                              uint64_t *res_postcopy_only)
{
}


static int vtd_save_iterate(QEMUFile *f, void *opaque)
{
    qemu_put_be64(f, 0);
    return 1; // for empty implementation, return 1 to let iterate go forward
}

static int vtd_save_complete_precopy(QEMUFile *f, void *opaque)
{
    IntelIOMMUState *s = opaque;
    uint64_t data_size = sizeof(*s);
    int ret;

    qemu_put_be64(f, data_size);
    qemu_put_buffer(f, (uint8_t *)s, data_size);

    ret = qemu_file_get_error(f);
    if (ret) {
        return ret;
    }

    return 0;
}

static int vtd_load_setup(QEMUFile *f, void *opaque)
{
    return 0;
}

static int vtd_load_cleanup(void *opaque)
{
    return 0;
}

static int vtd_load_state(QEMUFile *f, void *opaque, int version_id)
{
    IntelIOMMUState *iommu, *s = opaque;
    uint64_t data_size;
    int ret = 0;

    data_size = qemu_get_be64(f);
    if (data_size == 0) {
        return 0;
    }

    if (data_size != sizeof(*iommu)) {
        printf("%s ERROR data_size: %lu/%lu are incompatible!\n",
                __func__, data_size, sizeof(iommu));
        return -EINVAL;
    }

    iommu = g_malloc0(sizeof(*iommu));

    ret = qemu_get_buffer(f, (uint8_t *)iommu, data_size);
    if (ret == 0) {
        printf("%s Failed to get data\n", __func__);
        return -EINVAL;
    }

    ret = qemu_file_get_error(f);
    if (ret) {
        printf("%s - qemu_file_get_error, ret: %d\n", __func__, ret);
        return ret;
    }

    /* Config fileds in IntelIOMMUState per source configuration */
    s->root = iommu->root;
    s->intr_root = iommu->intr_root;
    s->iq = iommu->iq;
    s->intr_size = iommu->intr_size;
    s->iq_head = iommu->iq_head;
    s->iq_tail = iommu->iq_tail;
    s->iq_size = iommu->iq_size;
    s->next_frcd_reg = iommu->next_frcd_reg;
    memcpy(&s->csr, &iommu->csr, DMAR_REG_SIZE);
    s->iq_last_desc_type = iommu->iq_last_desc_type;
    s->dmar_enabled = iommu->dmar_enabled;
    s->qi_enabled = iommu->qi_enabled;
    s->intr_enabled = iommu->intr_enabled;
    s->intr_eime = iommu->intr_eime;
    s->iq_dw = iommu->iq_dw;
    s->pqa = iommu->pqa;
    s->prq_head = iommu->prq_head;
    s->prq_tail = iommu->prq_tail;
    s->prq_entry_size_order = iommu->prq_entry_size_order;
    s->prq_qsize = iommu->prq_qsize;
    s->prq_nb_entries = iommu->prq_nb_entries;
    s->prq_entry_count = iommu->prq_entry_count;
    memcpy(&s->vtd_pasid, &iommu->vtd_pasid, (1 << 20) * sizeof(VTDPASIDStoreEntry));

    vtd_post_load(s, 0);

    return 0;
}

static SaveVMHandlers savevm_vtd_handlers = {
    .save_setup = vtd_save_setup,
    .save_cleanup = vtd_save_cleanup,
    .save_live_pending = vtd_save_pending,
    .save_live_iterate = vtd_save_iterate,
    .save_live_complete_precopy = vtd_save_complete_precopy,
    .load_setup = vtd_load_setup,
    .load_cleanup = vtd_load_cleanup,
    .load_state = vtd_load_state,
};

static const MemoryRegionOps vtd_mem_ops = {
    .read = vtd_mem_read,
    .write = vtd_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static Property vtd_properties[] = {
    DEFINE_PROP_UINT32("version", IntelIOMMUState, version, 0),
    DEFINE_PROP_ON_OFF_AUTO("eim", IntelIOMMUState, intr_eim,
                            ON_OFF_AUTO_AUTO),
    DEFINE_PROP_BOOL("x-buggy-eim", IntelIOMMUState, buggy_eim, false),
    DEFINE_PROP_UINT8("aw-bits", IntelIOMMUState, aw_bits,
                      VTD_HOST_ADDRESS_WIDTH),
    DEFINE_PROP_BOOL("caching-mode", IntelIOMMUState, caching_mode, FALSE),
    DEFINE_PROP_STRING("x-scalable-mode", IntelIOMMUState, scalable_mode_str),
    DEFINE_PROP_BOOL("dma-drain", IntelIOMMUState, dma_drain, true),
    DEFINE_PROP_BOOL("pasid-migration", IntelIOMMUState, non_identical_pasid, false),
    DEFINE_PROP_END_OF_LIST(),
};

/* Read IRTE entry with specific index */
static int vtd_irte_get(IntelIOMMUState *iommu, uint16_t index,
                        VTD_IR_TableEntry *entry, uint16_t sid)
{
    static const uint16_t vtd_svt_mask[VTD_SQ_MAX] = \
        {0xffff, 0xfffb, 0xfff9, 0xfff8};
    dma_addr_t addr = 0x00;
    uint16_t mask, source_id;
    uint8_t bus, bus_max, bus_min;

    if (index >= iommu->intr_size) {
        error_report_once("%s: index too large: ind=0x%x",
                          __func__, index);
        return -VTD_FR_IR_INDEX_OVER;
    }

    addr = iommu->intr_root + index * sizeof(*entry);
    if (dma_memory_read(&address_space_memory, addr, entry,
                        sizeof(*entry))) {
        error_report_once("%s: read failed: ind=0x%x addr=0x%" PRIx64,
                          __func__, index, addr);
        return -VTD_FR_IR_ROOT_INVAL;
    }

    trace_vtd_ir_irte_get(index, le64_to_cpu(entry->data[1]),
                          le64_to_cpu(entry->data[0]));

    if (!entry->irte.present) {
        error_report_once("%s: detected non-present IRTE "
                          "(index=%u, high=0x%" PRIx64 ", low=0x%" PRIx64 ")",
                          __func__, index, le64_to_cpu(entry->data[1]),
                          le64_to_cpu(entry->data[0]));
        return -VTD_FR_IR_ENTRY_P;
    }

    if (entry->irte.__reserved_0 || entry->irte.__reserved_1 ||
        entry->irte.__reserved_2) {
        error_report_once("%s: detected non-zero reserved IRTE "
                          "(index=%u, high=0x%" PRIx64 ", low=0x%" PRIx64 ")",
                          __func__, index, le64_to_cpu(entry->data[1]),
                          le64_to_cpu(entry->data[0]));
        return -VTD_FR_IR_IRTE_RSVD;
    }

    if (sid != X86_IOMMU_SID_INVALID) {
        /* Validate IRTE SID */
        source_id = le32_to_cpu(entry->irte.source_id);
        switch (entry->irte.sid_vtype) {
        case VTD_SVT_NONE:
            break;

        case VTD_SVT_ALL:
            mask = vtd_svt_mask[entry->irte.sid_q];
            if ((source_id & mask) != (sid & mask)) {
                error_report_once("%s: invalid IRTE SID "
                                  "(index=%u, sid=%u, source_id=%u)",
                                  __func__, index, sid, source_id);
                return -VTD_FR_IR_SID_ERR;
            }
            break;

        case VTD_SVT_BUS:
            bus_max = source_id >> 8;
            bus_min = source_id & 0xff;
            bus = sid >> 8;
            if (bus > bus_max || bus < bus_min) {
                error_report_once("%s: invalid SVT_BUS "
                                  "(index=%u, bus=%u, min=%u, max=%u)",
                                  __func__, index, bus, bus_min, bus_max);
                return -VTD_FR_IR_SID_ERR;
            }
            break;

        default:
            error_report_once("%s: detected invalid IRTE SVT "
                              "(index=%u, type=%d)", __func__,
                              index, entry->irte.sid_vtype);
            /* Take this as verification failure. */
            return -VTD_FR_IR_SID_ERR;
        }
    }

    return 0;
}

/* Fetch IRQ information of specific IR index */
static int vtd_remap_irq_get(IntelIOMMUState *iommu, uint16_t index,
                             X86IOMMUIrq *irq, uint16_t sid)
{
    VTD_IR_TableEntry irte = {};
    int ret = 0;

    ret = vtd_irte_get(iommu, index, &irte, sid);
    if (ret) {
        return ret;
    }

    irq->trigger_mode = irte.irte.trigger_mode;
    irq->vector = irte.irte.vector;
    irq->delivery_mode = irte.irte.delivery_mode;
    irq->dest = le32_to_cpu(irte.irte.dest_id);
    if (!iommu->intr_eime) {
#define  VTD_IR_APIC_DEST_MASK         (0xff00ULL)
#define  VTD_IR_APIC_DEST_SHIFT        (8)
        irq->dest = (irq->dest & VTD_IR_APIC_DEST_MASK) >>
            VTD_IR_APIC_DEST_SHIFT;
    }
    irq->dest_mode = irte.irte.dest_mode;
    irq->redir_hint = irte.irte.redir_hint;

    trace_vtd_ir_remap(index, irq->trigger_mode, irq->vector,
                       irq->delivery_mode, irq->dest, irq->dest_mode);

    return 0;
}

/* Interrupt remapping for MSI/MSI-X entry */
static int vtd_interrupt_remap_msi(IntelIOMMUState *iommu,
                                   MSIMessage *origin,
                                   MSIMessage *translated,
                                   uint16_t sid)
{
    int ret = 0;
    VTD_IR_MSIAddress addr;
    uint16_t index;
    X86IOMMUIrq irq = {};

    assert(origin && translated);

    trace_vtd_ir_remap_msi_req(origin->address, origin->data);

    if (!iommu || !iommu->intr_enabled) {
        memcpy(translated, origin, sizeof(*origin));
        goto out;
    }

    if (origin->address & VTD_MSI_ADDR_HI_MASK) {
        error_report_once("%s: MSI address high 32 bits non-zero detected: "
                          "address=0x%" PRIx64, __func__, origin->address);
        return -VTD_FR_IR_REQ_RSVD;
    }

    addr.data = origin->address & VTD_MSI_ADDR_LO_MASK;
    if (addr.addr.__head != 0xfee) {
        error_report_once("%s: MSI address low 32 bit invalid: 0x%" PRIx32,
                          __func__, addr.data);
        return -VTD_FR_IR_REQ_RSVD;
    }

    /* This is compatible mode. */
    if (addr.addr.int_mode != VTD_IR_INT_FORMAT_REMAP) {
        memcpy(translated, origin, sizeof(*origin));
        goto out;
    }

    index = addr.addr.index_h << 15 | le16_to_cpu(addr.addr.index_l);

#define  VTD_IR_MSI_DATA_SUBHANDLE       (0x0000ffff)
#define  VTD_IR_MSI_DATA_RESERVED        (0xffff0000)

    if (addr.addr.sub_valid) {
        /* See VT-d spec 5.1.2.2 and 5.1.3 on subhandle */
        index += origin->data & VTD_IR_MSI_DATA_SUBHANDLE;
    }

    ret = vtd_remap_irq_get(iommu, index, &irq, sid);
    if (ret) {
        return ret;
    }

    if (addr.addr.sub_valid) {
        trace_vtd_ir_remap_type("MSI");
        if (origin->data & VTD_IR_MSI_DATA_RESERVED) {
            error_report_once("%s: invalid IR MSI "
                              "(sid=%u, address=0x%" PRIx64
                              ", data=0x%" PRIx32 ")",
                              __func__, sid, origin->address, origin->data);
            return -VTD_FR_IR_REQ_RSVD;
        }
    } else {
        uint8_t vector = origin->data & 0xff;
        uint8_t trigger_mode = (origin->data >> MSI_DATA_TRIGGER_SHIFT) & 0x1;

        trace_vtd_ir_remap_type("IOAPIC");
        /* IOAPIC entry vector should be aligned with IRTE vector
         * (see vt-d spec 5.1.5.1). */
        if (vector != irq.vector) {
            trace_vtd_warn_ir_vector(sid, index, vector, irq.vector);
        }

        /* The Trigger Mode field must match the Trigger Mode in the IRTE.
         * (see vt-d spec 5.1.5.1). */
        if (trigger_mode != irq.trigger_mode) {
            trace_vtd_warn_ir_trigger(sid, index, trigger_mode,
                                      irq.trigger_mode);
        }
    }

    /*
     * We'd better keep the last two bits, assuming that guest OS
     * might modify it. Keep it does not hurt after all.
     */
    irq.msi_addr_last_bits = addr.addr.__not_care;

    /* Translate X86IOMMUIrq to MSI message */
    x86_iommu_irq_to_msi_message(&irq, translated);

out:
    trace_vtd_ir_remap_msi(origin->address, origin->data,
                           translated->address, translated->data);
    return 0;
}

static int vtd_int_remap(X86IOMMUState *iommu, MSIMessage *src,
                         MSIMessage *dst, uint16_t sid)
{
    return vtd_interrupt_remap_msi(INTEL_IOMMU_DEVICE(iommu),
                                   src, dst, sid);
}

static MemTxResult vtd_mem_ir_read(void *opaque, hwaddr addr,
                                   uint64_t *data, unsigned size,
                                   MemTxAttrs attrs)
{
    return MEMTX_OK;
}

static MemTxResult vtd_mem_ir_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    int ret = 0;
    MSIMessage from = {}, to = {};
    uint16_t sid = X86_IOMMU_SID_INVALID;

    from.address = (uint64_t) addr + VTD_INTERRUPT_ADDR_FIRST;
    from.data = (uint32_t) value;

    if (!attrs.unspecified) {
        /* We have explicit Source ID */
        sid = attrs.requester_id;
    }

    ret = vtd_interrupt_remap_msi(opaque, &from, &to, sid);
    if (ret) {
        /* TODO: report error */
        /* Drop this interrupt */
        return MEMTX_ERROR;
    }

    apic_get_class()->send_msi(&to);

    return MEMTX_OK;
}

static const MemoryRegionOps vtd_mem_ir_ops = {
    .read_with_attrs = vtd_mem_ir_read,
    .write_with_attrs = vtd_mem_ir_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/**
 * Fetch a VTDBus instance for given PCIBus. If no existing instance,
 * allocate one.
 */
static VTDBus *vtd_find_add_bus(IntelIOMMUState *s, PCIBus *bus)
{
    uintptr_t key = (uintptr_t)bus;
    VTDBus *vtd_bus = g_hash_table_lookup(s->vtd_as_by_busptr, &key);

    if (!vtd_bus) {
        uintptr_t *new_key = g_malloc(sizeof(*new_key));
        *new_key = (uintptr_t)bus;
        /* No corresponding free() */
        vtd_bus = g_malloc0(sizeof(VTDBus));
        vtd_bus->bus = bus;
        g_hash_table_insert(s->vtd_as_by_busptr, new_key, vtd_bus);
    }
    return vtd_bus;
}

VTDAddressSpace *vtd_find_add_as(IntelIOMMUState *s, PCIBus *bus, int devfn)
{
    VTDBus *vtd_bus;
    VTDAddressSpace *vtd_dev_as;
    char name[128];

    vtd_bus = vtd_find_add_bus(s, bus);
    vtd_dev_as = vtd_bus->dev_as[devfn];

    if (!vtd_dev_as) {
        snprintf(name, sizeof(name), "vtd-%02x.%x", PCI_SLOT(devfn),
                 PCI_FUNC(devfn));
        vtd_bus->dev_as[devfn] = vtd_dev_as = g_malloc0(sizeof(VTDAddressSpace));

        vtd_dev_as->bus = bus;
        vtd_dev_as->devfn = (uint8_t)devfn;
        vtd_dev_as->iommu_state = s;
        vtd_dev_as->context_cache_entry.context_cache_gen = 0;
        vtd_dev_as->iova_tree = iova_tree_new();

        memory_region_init(&vtd_dev_as->root, OBJECT(s), name, UINT64_MAX);
        address_space_init(&vtd_dev_as->as, &vtd_dev_as->root, "vtd-root");

        /*
         * Build the DMAR-disabled container with aliases to the
         * shared MRs.  Note that aliasing to a shared memory region
         * could help the memory API to detect same FlatViews so we
         * can have devices to share the same FlatView when DMAR is
         * disabled (either by not providing "intel_iommu=on" or with
         * "iommu=pt").  It will greatly reduce the total number of
         * FlatViews of the system hence VM runs faster.
         */
        memory_region_init_alias(&vtd_dev_as->nodmar, OBJECT(s),
                                 "vtd-nodmar", &s->mr_nodmar, 0,
                                 memory_region_size(&s->mr_nodmar));

        /*
         * Build the per-device DMAR-enabled container.
         *
         * TODO: currently we have per-device IOMMU memory region only
         * because we have per-device IOMMU notifiers for devices.  If
         * one day we can abstract the IOMMU notifiers out of the
         * memory regions then we can also share the same memory
         * region here just like what we've done above with the nodmar
         * region.
         */
        strcat(name, "-dmar");
        memory_region_init_iommu(&vtd_dev_as->iommu, sizeof(vtd_dev_as->iommu),
                                 TYPE_INTEL_IOMMU_MEMORY_REGION, OBJECT(s),
                                 name, UINT64_MAX);
        memory_region_init_alias(&vtd_dev_as->iommu_ir, OBJECT(s), "vtd-ir",
                                 &s->mr_ir, 0, memory_region_size(&s->mr_ir));
        memory_region_add_subregion_overlap(MEMORY_REGION(&vtd_dev_as->iommu),
                                            VTD_INTERRUPT_ADDR_FIRST,
                                            &vtd_dev_as->iommu_ir, 1);

        /*
         * Hook both the containers under the root container, we
         * switch between DMAR & noDMAR by enable/disable
         * corresponding sub-containers
         */
        memory_region_add_subregion_overlap(&vtd_dev_as->root, 0,
                                            MEMORY_REGION(&vtd_dev_as->iommu),
                                            0);
        memory_region_add_subregion_overlap(&vtd_dev_as->root, 0,
                                            &vtd_dev_as->nodmar, 0);

        vtd_switch_address_space(vtd_dev_as);
    }
    return vtd_dev_as;
}

static int vtd_dev_get_iommu_attr(PCIBus *bus, void *opaque, int32_t devfn,
                                  PCIDevice *dev, IOMMUAttr attr, void *data)
{
    IntelIOMMUState *s = opaque;
    int ret = 0;

    assert(0 <= devfn && devfn < PCI_DEVFN_MAX);

    if (dev && !strcmp(dev->name, "vfio-pci")) {
        bus = pci_get_bus(dev);
        devfn = dev->devfn;
    }

    switch (attr) {
    case IOMMU_WANT_NESTING:
    {
        bool *pdata = data;

        *pdata = s->scalable_modern ? true : false;
        break;
    }
    default:
        ret = -ENOENT;
    }
    return ret;
}


static bool vtd_check_nesting_info(IntelIOMMUState *s,
                                   struct iommu_nesting_info *info,
                                   struct iommu_nesting_info_vtd *vtd)
{
    return !((s->aw_bits != info->addr_width) ||
             ((s->host_cap ^ vtd->cap_reg) & VTD_CAP_MASK & s->host_cap) ||
             ((s->host_ecap ^ vtd->ecap_reg) & VTD_ECAP_MASK & s->host_ecap) ||
             (VTD_GET_PSS(s->host_ecap) != (info->pasid_bits - 1)));
}

/* Caller should hold iommu lock. */
static bool vtd_sync_nesting_info(IntelIOMMUState *s,
                                  struct iommu_nesting_info *info)
{
    struct iommu_nesting_info_vtd *vtd;
    uint64_t cap, ecap;

    vtd =  (struct iommu_nesting_info_vtd *) &info->vendor.vtd;

    if (s->cap_finalized) {
        return vtd_check_nesting_info(s, info, vtd);
    }

    if (s->aw_bits > info->addr_width) {
        error_report("User aw-bits: %u > host address width: %u",
                      s->aw_bits, info->addr_width);
        return false;
    }

    cap = s->host_cap & vtd->cap_reg & VTD_CAP_MASK;
    s->host_cap &= ~VTD_CAP_MASK;
    s->host_cap |= cap;

    ecap = s->host_ecap & vtd->ecap_reg & VTD_ECAP_MASK;
    s->host_ecap &= ~VTD_ECAP_MASK;
    s->host_ecap |= ecap;

    if ((VTD_ECAP_PASID & s->host_ecap) && info->pasid_bits &&
        (VTD_GET_PSS(s->host_ecap) > (info->pasid_bits - 1))) {
        s->host_ecap &= ~VTD_ECAP_PSS_MASK;
        s->host_ecap |= VTD_ECAP_PSS(info->pasid_bits - 1);
    }
    return true;
}

/*
 * virtual VT-d which wants nested needs to check the host IOMMU
 * nesting cap info behind the assigned devices. Thus that vIOMMU
 * could bind guest page table to host.
 */
static bool vtd_check_iommu_ctx(IntelIOMMUState *s,
                                HostIOMMUContext *iommu_ctx)
{
    struct iommu_nesting_info *info = iommu_ctx->info;
    uint32_t minsz, size;

    if (IOMMU_PASID_FORMAT_INTEL_VTD != info->format) {
        error_report("Format is not compatible for nesting!!!");
        return false;
    }

    size = sizeof(struct iommu_nesting_info_vtd);
    minsz = endof(struct iommu_nesting_info, flags);
    if (size > (info->argsz - minsz)) {
        /*
         * QEMU may have been using new linux-headers/iommu.h than
         * kernel supports, hence fail it.
         */
        error_report("IOMMU nesting cap is not compatible!!!");
        return false;
    }

    return vtd_sync_nesting_info(s, info);
}

static int vtd_dev_set_iommu_context(PCIBus *bus, void *opaque,
                                     int devfn, PCIDevice *dev,
                                     HostIOMMUContext *iommu_ctx)
{
    IntelIOMMUState *s = opaque;
    VTDBus *vtd_bus;
    VTDHostIOMMUContext *vtd_dev_icx;

    assert(0 <= devfn && devfn < PCI_DEVFN_MAX);

    if (dev && !strcmp(dev->name, "vfio-pci")) {
        bus = pci_get_bus(dev);
        devfn = dev->devfn;
    }

    /* only modern scalable supports unset_ioimmu_context */
    assert(s->scalable_modern);

    vtd_bus = vtd_find_add_bus(s, bus);

    vtd_iommu_lock(s);

    if (!vtd_check_iommu_ctx(s, iommu_ctx)) {
        vtd_iommu_unlock(s);
        return -ENOENT;
    }

    vtd_dev_icx = vtd_bus->dev_icx[devfn];

    assert(!vtd_dev_icx);

    vtd_bus->dev_icx[devfn] = vtd_dev_icx =
                    g_malloc0(sizeof(VTDHostIOMMUContext));
    vtd_dev_icx->vtd_bus = vtd_bus;
    vtd_dev_icx->devfn = (uint8_t)devfn;
    vtd_dev_icx->iommu_state = s;
    vtd_dev_icx->iommu_ctx = iommu_ctx;
    QLIST_INSERT_HEAD(&s->vtd_dev_icx_list, vtd_dev_icx, next);

    vtd_iommu_unlock(s);

    return 0;
}

static void vtd_dev_unset_iommu_context(PCIBus *bus, void *opaque,
                                        int devfn, PCIDevice *dev)
{
    IntelIOMMUState *s = opaque;
    VTDBus *vtd_bus;
    VTDHostIOMMUContext *vtd_dev_icx;

    assert(0 <= devfn && devfn < PCI_DEVFN_MAX);

    if (dev && !strcmp(dev->name, "vfio-pci")) {
        bus = pci_get_bus(dev);
        devfn = dev->devfn;
    }

    /* only modern scalable supports set_ioimmu_context */
    assert(s->scalable_modern);

    vtd_bus = vtd_find_add_bus(s, bus);

    vtd_iommu_lock(s);

    vtd_dev_icx = vtd_bus->dev_icx[devfn];
    if (vtd_dev_icx) {
        QLIST_REMOVE(vtd_dev_icx, next);
        g_free(vtd_dev_icx);
    }
    vtd_bus->dev_icx[devfn] = NULL;

    vtd_iommu_unlock(s);
}

static void vtd_assemble_pg_resp(struct iommu_page_response *pg_resp,
                                 VTDPageReqDsc prq, int code)
{
    pg_resp->argsz = sizeof(pg_resp);
    pg_resp->version = IOMMU_PAGE_RESP_VERSION_1;
    pg_resp->pasid = prq.pasid;
    pg_resp->code = code;
    pg_resp->flags = prq.pasid_present ? IOMMU_PAGE_RESP_PASID_VALID : 0;
    pg_resp->grpid = prq.prg_index;
    VTD_DEBUG("%s, PASID %d pg_resp flags %x\n", __func__, pg_resp->pasid, pg_resp->flags);
}

static int vtd_dev_report_iommu_fault(PCIBus *bus, void *opaque,
                                      int devfn, PCIDevice *dev,
                                      int count, struct iommu_fault *buf)
{
    uint8_t bus_num = pci_bus_num(bus);
    struct iommu_fault *fault = buf;
    IntelIOMMUState *s = opaque;
    VTDContextEntry ce;
    VTDPageReqDsc prq;
    int ret = 0;

    assert(0 <= devfn && devfn < PCI_DEVFN_MAX);

    /* only modern scalable supports set_ioimmu_context */
    assert(s->scalable_modern && s->scalable_mode);

    if (vtd_dev_to_context_entry(s, bus_num, devfn, &ce)) {
        return -ENOENT;
    }
    switch (fault->type) {
    case IOMMU_FAULT_DMA_UNRECOV:
        ret = 0;
        break;
    case IOMMU_FAULT_PAGE_REQ:
        /* Only support page request with PASID */
        if (!(IOMMU_FAULT_PAGE_REQUEST_PASID_VALID & fault->prm.flags)) {
           ret = -ENOTTY;
           break;
        }
        prq.type = 0x1; /* VT-d spec 3.0 defines it as 0x1*/
        prq.pasid_present = 1;
        prq.priv_data_present =(IOMMU_FAULT_PAGE_REQUEST_PRIV_DATA
                                               & fault->prm.flags) ? 1 : 0;
        prq.rsvd = 0x0;
        prq.rid = vtd_make_source_id(bus_num, devfn);
        ret = vtd_gpasid_find_by_host(s, fault->prm.pasid);
        if (ret < 0) {
            VTD_DEBUG("%s failed to find gpasid for hpasid: %d\n", __func__, fault->prm.flags);
            break;
        }
        prq.pasid = ret;
        prq.exe_req = (fault->prm.perm & IOMMU_FAULT_PERM_EXEC) ? 1 : 0;
        prq.pm_req = (fault->prm.perm & IOMMU_FAULT_PERM_PRIV) ? 1 : 0;
        prq.rsvd2 = 0x0;
        prq.rd_req = (fault->prm.perm & IOMMU_FAULT_PERM_READ) ? 1 : 0;
        prq.wr_req = (fault->prm.perm & IOMMU_FAULT_PERM_WRITE) ? 1 : 0;
        prq.lpig = (IOMMU_FAULT_PAGE_REQUEST_LAST_PAGE
                                                & fault->prm.flags) ? 1 : 0;
        prq.prg_index = fault->prm.grpid;
        prq.addr = fault->prm.addr >> VTD_PAGE_SHIFT; /* addr here is not pfn per intel-iommu driver (5.12) */
        prq.priv_data[0] = (IOMMU_FAULT_PAGE_REQUEST_PRIV_DATA
                    & fault->prm.flags) ? fault->prm.private_data[0] : 0x0;
        prq.priv_data[1] = (IOMMU_FAULT_PAGE_REQUEST_PRIV_DATA
                    & fault->prm.flags) ? fault->prm.private_data[1] : 0x0;
        /* If PRE bit of ce is disabled, we should send INVALID response */
        if (!(ce.val[0] & (1ULL << 4))) {
            struct iommu_page_response pg_resp;

            VTD_DEBUG("%s: ce PRE bit is 0, submit INVALID grp resp.\n", __func__);
            vtd_assemble_pg_resp(&pg_resp, prq, QI_RESP_INVALID);
            qemu_mutex_lock(&s->prq_lock);
            if (vtd_dev_send_page_response(s, bus, devfn, &pg_resp)) {
                error_report_once("%s: page response failed, resp_desc: "
                          "pasid=%d, flag=%x, code=%d", __func__,
                          pg_resp.pasid, pg_resp.flags, pg_resp.code);
                ret = -EINVAL;
            }
            qemu_mutex_unlock(&s->prq_lock);
            return ret;
        }
        vtd_report_page_request(s, &prq);
        qemu_mutex_lock(&s->prq_lock);
        if (prq.lpig) {
            VTDPRQEntry *prqe;

            prqe = g_malloc0(sizeof(*prqe));
            prqe->bus = bus;
            prqe->devfn = devfn;
            memcpy(&prqe->prq, &prq, sizeof(prq));
            /* track the received prqs */
            QLIST_INSERT_HEAD(&s->vtd_prq_list, prqe, next);
            VTD_DEBUG("%s,last page in group track in list, addr: 0x%lx\n",
                                          __func__, (unsigned long) prq.addr);
        }
        qemu_mutex_unlock(&s->prq_lock);
        ret = 0;
        break;
    default:
        error_report_once("%s, Unknown VT-d DMA Fault Type!!!", __func__);
        ret = -ENOENT;
    }

    return ret;
}

/* Unmap the whole range in the notifier's scope. */
static void vtd_address_space_unmap(VTDAddressSpace *as, IOMMUNotifier *n)
{
    hwaddr size, remain;
    hwaddr start = n->start;
    hwaddr end = n->end;
    IntelIOMMUState *s = as->iommu_state;
    DMAMap map;

    /*
     * If no IOMMU_NOTIFIER_UNMAP support, no need to go further
     * as bleow code requires to notify unmap.
     */
    if (!(n->notifier_flags & IOMMU_NOTIFIER_UNMAP)) {
        return;
    }
    /*
     * Note: all the codes in this function has a assumption that IOVA
     * bits are no more than VTD_MGAW bits (which is restricted by
     * VT-d spec), otherwise we need to consider overflow of 64 bits.
     */

    if (end > VTD_ADDRESS_SIZE(s->aw_bits) - 1) {
        /*
         * Don't need to unmap regions that is bigger than the whole
         * VT-d supported address space size
         */
        end = VTD_ADDRESS_SIZE(s->aw_bits) - 1;
    }

    assert(start <= end);
    size = remain = end - start + 1;

    while (remain >= VTD_PAGE_SIZE) {
        IOMMUTLBEvent event;
        uint64_t mask = dma_aligned_pow2_mask(start, end, s->aw_bits);
        uint64_t size = mask + 1;

        assert(size);

        event.type = IOMMU_NOTIFIER_UNMAP;
        event.entry.iova = start;
        event.entry.addr_mask = mask;
        event.entry.target_as = &address_space_memory;
        event.entry.perm = IOMMU_NONE;
        /* This field is meaningless for unmap */
        event.entry.translated_addr = 0;

        memory_region_notify_iommu_one(n, &event);

        start += size;
        remain -= size;
    }

    assert(!remain);

    trace_vtd_as_unmap_whole(pci_bus_num(as->bus),
                             VTD_PCI_SLOT(as->devfn),
                             VTD_PCI_FUNC(as->devfn),
                             n->start, size);

    map.iova = n->start;
    map.size = size;
    iova_tree_remove(as->iova_tree, &map);
}

static void vtd_address_space_unmap_all(IntelIOMMUState *s)
{
    VTDAddressSpace *vtd_as;
    IOMMUNotifier *n;

    QLIST_FOREACH(vtd_as, &s->vtd_as_with_notifiers, next) {
        IOMMU_NOTIFIER_FOREACH(n, &vtd_as->iommu) {
            vtd_address_space_unmap(vtd_as, n);
        }
    }
}

static void vtd_address_space_refresh_all(IntelIOMMUState *s)
{
    vtd_address_space_unmap_all(s);
    vtd_switch_address_space_all(s);
}

static int vtd_replay_hook(IOMMUTLBEvent *event, void *private)
{
    memory_region_notify_iommu_one(private, event);
    return 0;
}

static void vtd_iommu_replay(IOMMUMemoryRegion *iommu_mr, IOMMUNotifier *n)
{
    VTDAddressSpace *vtd_as = container_of(iommu_mr, VTDAddressSpace, iommu);
    IntelIOMMUState *s = vtd_as->iommu_state;
    uint8_t bus_n = pci_bus_num(vtd_as->bus);
    VTDContextEntry ce;

    /*
     * The replay can be triggered by either a invalidation or a newly
     * created entry. No matter what, we release existing mappings
     * (it means flushing caches for UNMAP-only registers).
     */
    vtd_address_space_unmap(vtd_as, n);

    if (vtd_dev_to_context_entry(s, bus_n, vtd_as->devfn, &ce) == 0) {
        trace_vtd_replay_ce_valid(s->root_scalable ? "scalable mode" :
                                  "legacy mode",
                                  bus_n, PCI_SLOT(vtd_as->devfn),
                                  PCI_FUNC(vtd_as->devfn),
                                  vtd_get_domain_id(s, &ce),
                                  ce.hi, ce.lo);
        if (vtd_as_has_map_notifier(vtd_as)) {
            /* This is required only for MAP typed notifiers */
            vtd_page_walk_info info = {
                .hook_fn = vtd_replay_hook,
                .private = (void *)n,
                .notify_unmap = false,
                .aw = s->aw_bits,
                .as = vtd_as,
                .domain_id = vtd_get_domain_id(s, &ce),
            };

            vtd_page_walk(s, &ce, 0, ~0ULL, &info);
        }
    } else {
        trace_vtd_replay_ce_invalid(bus_n, PCI_SLOT(vtd_as->devfn),
                                    PCI_FUNC(vtd_as->devfn));
    }

    return;
}

/* Do the initialization. It will also be called when reset, so pay
 * attention when adding new initialization stuff.
 */
static void vtd_init(IntelIOMMUState *s)
{
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);

    memset(s->csr, 0, DMAR_REG_SIZE);
    memset(s->wmask, 0, DMAR_REG_SIZE);
    memset(s->w1cmask, 0, DMAR_REG_SIZE);
    memset(s->womask, 0, DMAR_REG_SIZE);

    s->root = 0;
    s->root_scalable = false;
    s->dmar_enabled = false;
    s->intr_enabled = false;
    s->iq_head = 0;
    s->iq_tail = 0;
    s->iq = 0;
    s->iq_size = 0;
    s->qi_enabled = false;
    s->iq_last_desc_type = VTD_INV_DESC_NONE;
    s->iq_dw = false;
    s->next_frcd_reg = 0;
    s->cap = VTD_CAP_FRO | VTD_CAP_NFR | VTD_CAP_ND |
             VTD_CAP_MAMV | VTD_CAP_PSI | VTD_CAP_SLLPS |
             VTD_CAP_SAGAW_39bit | VTD_CAP_MGAW(s->aw_bits);
    if (s->dma_drain) {
        s->cap |= VTD_CAP_DRAIN;
    }
    if (s->aw_bits == VTD_HOST_AW_48BIT) {
        s->cap |= VTD_CAP_SAGAW_48bit;
    }
    s->ecap = VTD_ECAP_QI | VTD_ECAP_IRO;

    /*
     * Rsvd field masks for spte
     */
    vtd_spte_rsvd[0] = ~0ULL;
    vtd_spte_rsvd[1] = VTD_SPTE_PAGE_L1_RSVD_MASK(s->aw_bits,
                                                  x86_iommu->dt_supported);
    vtd_spte_rsvd[2] = VTD_SPTE_PAGE_L2_RSVD_MASK(s->aw_bits);
    vtd_spte_rsvd[3] = VTD_SPTE_PAGE_L3_RSVD_MASK(s->aw_bits);
    vtd_spte_rsvd[4] = VTD_SPTE_PAGE_L4_RSVD_MASK(s->aw_bits);

    vtd_spte_rsvd_large[2] = VTD_SPTE_LPAGE_L2_RSVD_MASK(s->aw_bits,
                                                         x86_iommu->dt_supported);
    vtd_spte_rsvd_large[3] = VTD_SPTE_LPAGE_L3_RSVD_MASK(s->aw_bits,
                                                         x86_iommu->dt_supported);

    if (x86_iommu_ir_supported(x86_iommu)) {
        s->ecap |= VTD_ECAP_IR | VTD_ECAP_MHMV;
        if (s->intr_eim == ON_OFF_AUTO_ON) {
            s->ecap |= VTD_ECAP_EIM;
        }
        assert(s->intr_eim != ON_OFF_AUTO_AUTO);
    }

    if (x86_iommu->dt_supported) {
        s->ecap |= VTD_ECAP_DT;
    }

    if (x86_iommu->pt_supported) {
        s->ecap |= VTD_ECAP_PT;
    }

    if (s->caching_mode) {
        s->cap |= VTD_CAP_CM;
    }

    /* TODO: read cap/ecap from host to decide which cap to be exposed. */
    if (s->scalable_mode && !s->scalable_modern) {
        s->ecap |= VTD_ECAP_SMTS | VTD_ECAP_SRS | VTD_ECAP_SLTS;
    } else if (s->scalable_mode && s->scalable_modern) {
        s->ecap |= VTD_ECAP_SMTS | VTD_ECAP_SRS | VTD_ECAP_PASID |
                   VTD_ECAP_PSS(VTD_PASID_SS) | VTD_ECAP_VCS  | VTD_ECAP_PRS |
                   VTD_ECAP_EAFS;
        if (s->aw_bits == VTD_HOST_AW_48BIT) {
            s->ecap |= VTD_ECAP_FLTS;
            s->cap |= VTD_CAP_FL1GP | VTD_CAP_FL5LP;
        }
        s->vccap |= VTD_VCCAP_PAS;
    }

    if (!s->cap_finalized) {
        s->host_cap = s->cap;
        s->host_ecap = s->ecap;
    } else {
        s->cap = s->host_cap;
        s->ecap = s->host_ecap;
    }

    vtd_reset_caches(s);

    /* Define registers with default values and bit semantics */
    vtd_define_long(s, DMAR_VER_REG, 0x10UL, 0, 0);
    vtd_define_quad(s, DMAR_CAP_REG, s->cap, 0, 0);
    vtd_define_quad(s, DMAR_ECAP_REG, s->ecap, 0, 0);
    vtd_define_long(s, DMAR_GCMD_REG, 0, 0xff800000UL, 0);
    vtd_define_long_wo(s, DMAR_GCMD_REG, 0xff800000UL);
    vtd_define_long(s, DMAR_GSTS_REG, 0, 0, 0);
    vtd_define_quad(s, DMAR_RTADDR_REG, 0, 0xfffffffffffffc00ULL, 0);
    vtd_define_quad(s, DMAR_CCMD_REG, 0, 0xe0000003ffffffffULL, 0);
    vtd_define_quad_wo(s, DMAR_CCMD_REG, 0x3ffff0000ULL);

    /* Advanced Fault Logging not supported */
    vtd_define_long(s, DMAR_FSTS_REG, 0, 0, 0x11UL);
    vtd_define_long(s, DMAR_FECTL_REG, 0x80000000UL, 0x80000000UL, 0);
    vtd_define_long(s, DMAR_FEDATA_REG, 0, 0x0000ffffUL, 0);
    vtd_define_long(s, DMAR_FEADDR_REG, 0, 0xfffffffcUL, 0);

    /* Treated as RsvdZ when EIM in ECAP_REG is not supported
     * vtd_define_long(s, DMAR_FEUADDR_REG, 0, 0xffffffffUL, 0);
     */
    vtd_define_long(s, DMAR_FEUADDR_REG, 0, 0, 0);

    /* Treated as RO for implementations that PLMR and PHMR fields reported
     * as Clear in the CAP_REG.
     * vtd_define_long(s, DMAR_PMEN_REG, 0, 0x80000000UL, 0);
     */
    vtd_define_long(s, DMAR_PMEN_REG, 0, 0, 0);

    vtd_define_quad(s, DMAR_IQH_REG, 0, 0, 0);
    vtd_define_quad(s, DMAR_IQT_REG, 0, 0x7fff0ULL, 0);
    vtd_define_quad(s, DMAR_IQA_REG, 0, 0xfffffffffffff807ULL, 0);
    vtd_define_long(s, DMAR_ICS_REG, 0, 0, 0x1UL);
    vtd_define_long(s, DMAR_IECTL_REG, 0x80000000UL, 0x80000000UL, 0);
    vtd_define_long(s, DMAR_IEDATA_REG, 0, 0xffffffffUL, 0);
    vtd_define_long(s, DMAR_IEADDR_REG, 0, 0xfffffffcUL, 0);
    /* Treadted as RsvdZ when EIM in ECAP_REG is not supported */
    vtd_define_long(s, DMAR_IEUADDR_REG, 0, 0, 0);

    /* Page Request Service registers */
    vtd_define_quad(s, DMAR_PQH_REG, 0, 0, 0);
    vtd_define_quad(s, DMAR_PQT_REG, 0, 0x7fff0ULL, 0);
    vtd_define_quad(s, DMAR_PQA_REG, 0, 0xfffffffffffff007ULL, 0);
    vtd_define_long(s, DMAR_PRS_REG, 0, 0, 0x1UL);
    vtd_define_long(s, DMAR_PECTL_REG, 0x80000000UL, 0x80000000UL, 0);
    vtd_define_long(s, DMAR_PEDATA_REG, 0, 0xffffffffUL, 0);
    vtd_define_long(s, DMAR_PEADDR_REG, 0, 0xfffffffcUL, 0);
    /* Treadted as RsvdZ when EIM in ECAP_REG is not supported */
    vtd_define_long(s, DMAR_PEUADDR_REG, 0, 0, 0);


    /* IOTLB registers */
    vtd_define_quad(s, DMAR_IOTLB_REG, 0, 0Xb003ffff00000000ULL, 0);
    vtd_define_quad(s, DMAR_IVA_REG, 0, 0xfffffffffffff07fULL, 0);
    vtd_define_quad_wo(s, DMAR_IVA_REG, 0xfffffffffffff07fULL);

    /* Fault Recording Registers, 128-bit */
    vtd_define_quad(s, DMAR_FRCD_REG_0_0, 0, 0, 0);
    vtd_define_quad(s, DMAR_FRCD_REG_0_2, 0, 0, 0x8000000000000000ULL);

    /*
     * Interrupt remapping registers.
     */
    vtd_define_quad(s, DMAR_IRTA_REG, 0, 0xfffffffffffff80fULL, 0);

    /*
     * Virtual Command Definitions
     */
    vtd_define_quad(s, DMAR_VCCAP_REG, s->vccap, 0, 0);
    vtd_define_quad(s, DMAR_VCMD_REG, 0, 0xffffffffffffffffULL, 0);
    vtd_define_quad(s, DMAR_VCRSP_REG, 0, 0, 0);
}

/* Should not reset address_spaces when reset because devices will still use
 * the address space they got at first (won't ask the bus again).
 */
static void vtd_reset(DeviceState *dev)
{
    IntelIOMMUState *s = INTEL_IOMMU_DEVICE(dev);

    vtd_init(s);
    vtd_address_space_refresh_all(s);
}

static AddressSpace *vtd_host_dma_iommu(PCIBus *bus, void *opaque,
                                        int devfn, PCIDevice *dev)
{
    IntelIOMMUState *s = opaque;
    VTDAddressSpace *vtd_as;

    assert(0 <= devfn && devfn < PCI_DEVFN_MAX);

    /*
     * If assigned devices lays behind a PCIe-to-PCI bridge, the pci
     * layer of qemu makes these devices share the same address
     * space since they will be aliased. However, vIOMMU should manage
     * them separately since the devices should have its own bdf.
     * Only detected vfio device so far. In future, vdpa device may
     * also be checked.
     */
    if (dev && !strcmp(dev->name, "vfio-pci")) {
        bus = pci_get_bus(dev);
        devfn = dev->devfn;
    }

    vtd_as = vtd_find_add_as(s, bus, devfn);
    return &vtd_as->as;
}

static PCIIOMMUOps vtd_iommu_ops = {
    .get_address_space = vtd_host_dma_iommu,
    .get_iommu_attr = vtd_dev_get_iommu_attr,
    .set_iommu_context = vtd_dev_set_iommu_context,
    .unset_iommu_context = vtd_dev_unset_iommu_context,
    .report_iommu_fault = vtd_dev_report_iommu_fault,
};

static bool vtd_decide_config(IntelIOMMUState *s, Error **errp)
{
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);

    if (s->intr_eim == ON_OFF_AUTO_ON && !x86_iommu_ir_supported(x86_iommu)) {
        error_setg(errp, "eim=on cannot be selected without intremap=on");
        return false;
    }

    if (s->intr_eim == ON_OFF_AUTO_AUTO) {
        s->intr_eim = (kvm_irqchip_in_kernel() || s->buggy_eim)
                      && x86_iommu_ir_supported(x86_iommu) ?
                                              ON_OFF_AUTO_ON : ON_OFF_AUTO_OFF;
    }
    if (s->intr_eim == ON_OFF_AUTO_ON && !s->buggy_eim) {
        if (!kvm_irqchip_in_kernel()) {
            error_setg(errp, "eim=on requires accel=kvm,kernel-irqchip=split");
            return false;
        }
        if (!kvm_enable_x2apic()) {
            error_setg(errp, "eim=on requires support on the KVM side"
                             "(X2APIC_API, first shipped in v4.7)");
            return false;
        }
    }

    /* Currently only address widths supported are 39 and 48 bits */
    if ((s->aw_bits != VTD_HOST_AW_39BIT) &&
        (s->aw_bits != VTD_HOST_AW_48BIT)) {
        error_setg(errp, "Supported values for aw-bits are: %d, %d",
                   VTD_HOST_AW_39BIT, VTD_HOST_AW_48BIT);
        return false;
    }

    if (s->scalable_mode && !s->dma_drain) {
        error_setg(errp, "Need to set dma_drain for scalable mode");
        return false;
    }

    if (s->scalable_mode_str &&
        (strcmp(s->scalable_mode_str, "off") &&
         strcmp(s->scalable_mode_str, "modern") &&
         strcmp(s->scalable_mode_str, "legacy"))) {
        error_setg(errp, "Invalid x-scalable-mode config,"
                         "Please use \"modern\", \"legacy\" or \"off\"");
        return false;
    }

    if (s->scalable_mode_str &&
        !strcmp(s->scalable_mode_str, "legacy")) {
        s->scalable_mode = true;
        s->scalable_modern = false;
    } else if (s->scalable_mode_str &&
        !strcmp(s->scalable_mode_str, "modern")) {
        if (ioasid_fd < 0) {
            int fd, version;
            struct ioasid_info info;

            fd = qemu_open_old("/dev/ioasid", O_RDWR);
            if (fd < 0) {
                error_setg(errp, "Failed to open /dev/ioasid, %m");
                return false;
            }

            version = ioctl(fd, IOASID_GET_API_VERSION);
            if (version != IOASID_API_VERSION) {
                error_setg(errp, "supported ioasid version: %d, "
                           "reported version: %d", IOASID_API_VERSION, version);
                qemu_close(fd);
                return false;
            }

            memset(&info, 0x0, sizeof(info));
            info.argsz = sizeof(info);
            if (ioctl(fd, IOASID_GET_INFO, &info)) {
                error_setg(errp, "Failed to get ioasid info, %m");
                qemu_close(fd);
                return false;
            }

            if ((VTD_PASID_SS + 1) > info.ioasid_bits) {
                error_setg(errp, "supported pasid bits: %u, reported pasid "
                           "bits: %u", VTD_PASID_SS + 1, info.ioasid_bits);
                qemu_close(fd);
                return false;
            }

            ioasid_fd = fd;
            ioasid_bits = info.ioasid_bits;
        }
        s->ioasid_fd = ioasid_fd;
        s->ioasid_bits = ioasid_bits;
        s->scalable_mode = true;
        s->scalable_modern = true;
    } else {
        s->scalable_mode = false;
        s->scalable_modern = false;
    }

    if (s->non_identical_pasid && !s->scalable_modern) {
        error_setg(errp, "Non identical PASID only be available for scalable modern mode");
        return false;
    }

    return true;
}

static void vtd_refresh_capability_reg(IntelIOMMUState *s)
{
    vtd_set_quad(s, DMAR_CAP_REG, s->cap);
    vtd_set_quad(s, DMAR_ECAP_REG, s->ecap);
}

static int vtd_machine_done_notify_one(Object *child, void *unused)
{
    IntelIOMMUState *iommu = INTEL_IOMMU_DEVICE(x86_iommu_get_default());

    /*
     * We hard-coded here because vfio-pci is the only special case
     * here.  Let's be more elegant in the future when we can, but so
     * far there seems to be no better way.
     */
    if (object_dynamic_cast(child, "vfio-pci") && !iommu->caching_mode) {
        vtd_panic_require_caching_mode();
    }

    vtd_iommu_lock(iommu);
    iommu->cap = iommu->host_cap & iommu->cap;
    iommu->ecap = iommu->host_ecap & iommu->ecap;
    if (!iommu->cap_finalized) {
        iommu->cap_finalized = true;
    }

    vtd_refresh_capability_reg(iommu);
    vtd_iommu_unlock(iommu);
    return 0;
}

static void vtd_machine_done_hook(Notifier *notifier, void *unused)
{
    object_child_foreach_recursive(object_get_root(),
                                   vtd_machine_done_notify_one, NULL);
}

static Notifier vtd_machine_done_notify = {
    .notify = vtd_machine_done_hook,
};

static void vtd_migration_probe(IntelIOMMUState *s, Error **errp)
{
    register_savevm_live("intel-vtd-3.2", -1, 1, &savevm_vtd_handlers,
                         s);
}

static void vtd_realize(DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    PCMachineState *pcms = PC_MACHINE(ms);
    X86MachineState *x86ms = X86_MACHINE(ms);
    PCIBus *bus = pcms->bus;
    IntelIOMMUState *s = INTEL_IOMMU_DEVICE(dev);
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(dev);

    x86_iommu->type = TYPE_INTEL;

    if (!vtd_decide_config(s, errp)) {
        return;
    }

    QLIST_INIT(&s->vtd_as_with_notifiers);
    QLIST_INIT(&s->vtd_dev_icx_list);
    QLIST_INIT(&s->vtd_prq_list);
    qemu_mutex_init(&s->iommu_lock);
    qemu_mutex_init(&s->prq_lock);
    s->cap_finalized = false;
    memset(s->vtd_as_by_bus_num, 0, sizeof(s->vtd_as_by_bus_num));
    memory_region_init_io(&s->csrmem, OBJECT(s), &vtd_mem_ops, s,
                          "intel_iommu", DMAR_REG_SIZE);

    /* Create the shared memory regions by all devices */
    memory_region_init(&s->mr_nodmar, OBJECT(s), "vtd-nodmar",
                       UINT64_MAX);
    memory_region_init_io(&s->mr_ir, OBJECT(s), &vtd_mem_ir_ops,
                          s, "vtd-ir", VTD_INTERRUPT_ADDR_SIZE);
    memory_region_init_alias(&s->mr_sys_alias, OBJECT(s),
                             "vtd-sys-alias", get_system_memory(), 0,
                             memory_region_size(get_system_memory()));
    memory_region_add_subregion_overlap(&s->mr_nodmar, 0,
                                        &s->mr_sys_alias, 0);
    memory_region_add_subregion_overlap(&s->mr_nodmar,
                                        VTD_INTERRUPT_ADDR_FIRST,
                                        &s->mr_ir, 1);

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->csrmem);
    /* No corresponding destroy */
    s->iotlb = g_hash_table_new_full(vtd_uint64_hash, vtd_uint64_equal,
                                     g_free, g_free);
    s->p_iotlb = g_hash_table_new_full(&g_str_hash, &g_str_equal,
                                       g_free, g_free);
    s->vtd_as_by_busptr = g_hash_table_new_full(vtd_uint64_hash, vtd_uint64_equal,
                                              g_free, g_free);
    s->vtd_pasid_as = g_hash_table_new_full(vtd_pasid_as_key_hash,
                                            vtd_pasid_as_key_equal,
                                            g_free, g_free);
    s->next_idx = 0;;
    vtd_init(s);
    if (likely(!(s->ecap & VTD_ECAP_RPS))) {
        VTDPASIDStoreEntry *entry;

        vtd_iommu_lock(s);
        entry = vtd_pasid_alloc_idx(s);
        if (entry && entry->gpasid == 0) {
            entry->hpasid = 0;
        } else {
            error_setg(errp, "Failed to reserve gPASID 0 for scalable mode");
        }
        vtd_iommu_unlock(s);
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s), 0, Q35_HOST_BRIDGE_IOMMU_ADDR);
    pci_setup_iommu(bus, &vtd_iommu_ops, dev);
    /* Pseudo address space under root PCI bus. */
    x86ms->ioapic_as = vtd_host_dma_iommu(bus, s, Q35_PSEUDO_DEVFN_IOAPIC, NULL);
    qemu_add_machine_init_done_notifier(&vtd_machine_done_notify);
    vtd_migration_probe(s, errp);
}

static void vtd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    X86IOMMUClass *x86_class = X86_IOMMU_DEVICE_CLASS(klass);

    dc->reset = vtd_reset;
    device_class_set_props(dc, vtd_properties);
    dc->hotpluggable = false;
    x86_class->realize = vtd_realize;
    x86_class->int_remap = vtd_int_remap;
    /* Supported by the pc-q35-* machine types */
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "Intel IOMMU (VT-d) DMA Remapping device";
}

static const TypeInfo vtd_info = {
    .name          = TYPE_INTEL_IOMMU_DEVICE,
    .parent        = TYPE_X86_IOMMU_DEVICE,
    .instance_size = sizeof(IntelIOMMUState),
    .class_init    = vtd_class_init,
};

static void vtd_iommu_memory_region_class_init(ObjectClass *klass,
                                                     void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = vtd_iommu_translate;
    imrc->notify_flag_changed = vtd_iommu_notify_flag_changed;
    imrc->replay = vtd_iommu_replay;
}

static const TypeInfo vtd_iommu_memory_region_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_INTEL_IOMMU_MEMORY_REGION,
    .class_init = vtd_iommu_memory_region_class_init,
};

static void vtd_register_types(void)
{
    type_register_static(&vtd_info);
    type_register_static(&vtd_iommu_memory_region_info);
}

type_init(vtd_register_types)
