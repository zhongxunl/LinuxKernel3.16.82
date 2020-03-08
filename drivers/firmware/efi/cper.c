/*
 * UEFI Common Platform Error Record (CPER) support
 *
 * Copyright (C) 2010, Intel Corp.
 *	Author: Huang Ying <ying.huang@intel.com>
 *
 * CPER is the format used to describe platform hardware error by
 * various tables, such as ERST, BERT and HEST etc.
 *
 * For more information about CPER, please refer to Appendix N of UEFI
 * Specification version 2.4.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/time.h>
#include <linux/cper.h>
#include <linux/dmi.h>
#include <linux/acpi.h>
#include <linux/pci.h>
#include <linux/aer.h>

#define INDENT_SP	" "
/*
 * CPER record ID need to be unique even after reboot, because record
 * ID is used as index for ERST storage, while CPER records from
 * multiple boot may co-exist in ERST.
 */
u64 cper_next_record_id(void)
{
	static atomic64_t seq;

	if (!atomic64_read(&seq))
		atomic64_set(&seq, ((u64)get_seconds()) << 32);

	return atomic64_inc_return(&seq);
}
EXPORT_SYMBOL_GPL(cper_next_record_id);

static const char *cper_severity_strs[] = {
	"recoverable",
	"fatal",
	"corrected",
	"info",
};

static const char *cper_severity_str(unsigned int severity)
{
	return severity < ARRAY_SIZE(cper_severity_strs) ?
		cper_severity_strs[severity] : "unknown";
}

/*
 * cper_print_bits - print strings for set bits
 * @pfx: prefix for each line, including log level and prefix string
 * @bits: bit mask
 * @strs: string array, indexed by bit position
 * @strs_size: size of the string array: @strs
 *
 * For each set bit in @bits, print the corresponding string in @strs.
 * If the output length is longer than 80, multiple line will be
 * printed, with @pfx is printed at the beginning of each line.
 */
void cper_print_bits(const char *pfx, unsigned int bits,
		     const char * const strs[], unsigned int strs_size)
{
	int i, len = 0;
	const char *str;
	char buf[84];

	for (i = 0; i < strs_size; i++) {
		if (!(bits & (1U << i)))
			continue;
		str = strs[i];
		if (!str)
			continue;
		if (len && len + strlen(str) + 2 > 80) {
			printk("%s\n", buf);
			len = 0;
		}
		if (!len)
			len = snprintf(buf, sizeof(buf), "%s%s", pfx, str);
		else
			len += snprintf(buf+len, sizeof(buf)-len, ", %s", str);
	}
	if (len)
		printk("%s\n", buf);
}

static const char * const cper_proc_type_strs[] = {
	"IA32/X64",
	"IA64",
};

static const char * const cper_proc_isa_strs[] = {
	"IA32",
	"IA64",
	"X64",
};

static const char * const cper_proc_error_type_strs[] = {
	"cache error",
	"TLB error",
	"bus error",
	"micro-architectural error",
};

static const char * const cper_proc_op_strs[] = {
	"unknown or generic",
	"data read",
	"data write",
	"instruction execution",
};

static const char * const cper_proc_flag_strs[] = {
	"restartable",
	"precise IP",
	"overflow",
	"corrected",
};

static void cper_print_proc_generic(const char *pfx,
				    const struct cper_sec_proc_generic *proc)
{
	if (proc->validation_bits & CPER_PROC_VALID_TYPE)
		printk("%s""processor_type: %d, %s\n", pfx, proc->proc_type,
		       proc->proc_type < ARRAY_SIZE(cper_proc_type_strs) ?
		       cper_proc_type_strs[proc->proc_type] : "unknown");
	if (proc->validation_bits & CPER_PROC_VALID_ISA)
		printk("%s""processor_isa: %d, %s\n", pfx, proc->proc_isa,
		       proc->proc_isa < ARRAY_SIZE(cper_proc_isa_strs) ?
		       cper_proc_isa_strs[proc->proc_isa] : "unknown");
	if (proc->validation_bits & CPER_PROC_VALID_ERROR_TYPE) {
		printk("%s""error_type: 0x%02x\n", pfx, proc->proc_error_type);
		cper_print_bits(pfx, proc->proc_error_type,
				cper_proc_error_type_strs,
				ARRAY_SIZE(cper_proc_error_type_strs));
	}
	if (proc->validation_bits & CPER_PROC_VALID_OPERATION)
		printk("%s""operation: %d, %s\n", pfx, proc->operation,
		       proc->operation < ARRAY_SIZE(cper_proc_op_strs) ?
		       cper_proc_op_strs[proc->operation] : "unknown");
	if (proc->validation_bits & CPER_PROC_VALID_FLAGS) {
		printk("%s""flags: 0x%02x\n", pfx, proc->flags);
		cper_print_bits(pfx, proc->flags, cper_proc_flag_strs,
				ARRAY_SIZE(cper_proc_flag_strs));
	}
	if (proc->validation_bits & CPER_PROC_VALID_LEVEL)
		printk("%s""level: %d\n", pfx, proc->level);
	if (proc->validation_bits & CPER_PROC_VALID_VERSION)
		printk("%s""version_info: 0x%016llx\n", pfx, proc->cpu_version);
	if (proc->validation_bits & CPER_PROC_VALID_ID)
		printk("%s""processor_id: 0x%016llx\n", pfx, proc->proc_id);
	if (proc->validation_bits & CPER_PROC_VALID_TARGET_ADDRESS)
		printk("%s""target_address: 0x%016llx\n",
		       pfx, proc->target_addr);
	if (proc->validation_bits & CPER_PROC_VALID_REQUESTOR_ID)
		printk("%s""requestor_id: 0x%016llx\n",
		       pfx, proc->requestor_id);
	if (proc->validation_bits & CPER_PROC_VALID_RESPONDER_ID)
		printk("%s""responder_id: 0x%016llx\n",
		       pfx, proc->responder_id);
	if (proc->validation_bits & CPER_PROC_VALID_IP)
		printk("%s""IP: 0x%016llx\n", pfx, proc->ip);
}

static const char *cper_mem_err_type_strs[] = {
	"unknown",
	"no error",
	"single-bit ECC",
	"multi-bit ECC",
	"single-symbol chipkill ECC",
	"multi-symbol chipkill ECC",
	"master abort",
	"target abort",
	"parity error",
	"watchdog timeout",
	"invalid address",
	"mirror Broken",
	"memory sparing",
	"scrub corrected error",
	"scrub uncorrected error",
	"physical memory map-out event",
};

static void cper_print_mem(const char *pfx, const struct cper_sec_mem_err *mem)
{
	if (mem->validation_bits & CPER_MEM_VALID_ERROR_STATUS)
		printk("%s""error_status: 0x%016llx\n", pfx, mem->error_status);
	if (mem->validation_bits & CPER_MEM_VALID_PA)
		printk("%s""physical_address: 0x%016llx\n",
		       pfx, mem->physical_addr);
	if (mem->validation_bits & CPER_MEM_VALID_PA_MASK)
		printk("%s""physical_address_mask: 0x%016llx\n",
		       pfx, mem->physical_addr_mask);
	if (mem->validation_bits & CPER_MEM_VALID_NODE)
		pr_debug("node: %d\n", mem->node);
	if (mem->validation_bits & CPER_MEM_VALID_CARD)
		pr_debug("card: %d\n", mem->card);
	if (mem->validation_bits & CPER_MEM_VALID_MODULE)
		pr_debug("module: %d\n", mem->module);
	if (mem->validation_bits & CPER_MEM_VALID_RANK_NUMBER)
		pr_debug("rank: %d\n", mem->rank);
	if (mem->validation_bits & CPER_MEM_VALID_BANK)
		pr_debug("bank: %d\n", mem->bank);
	if (mem->validation_bits & CPER_MEM_VALID_DEVICE)
		pr_debug("device: %d\n", mem->device);
	if (mem->validation_bits & CPER_MEM_VALID_ROW)
		pr_debug("row: %d\n", mem->row);
	if (mem->validation_bits & CPER_MEM_VALID_COLUMN)
		pr_debug("column: %d\n", mem->column);
	if (mem->validation_bits & CPER_MEM_VALID_BIT_POSITION)
		pr_debug("bit_position: %d\n", mem->bit_pos);
	if (mem->validation_bits & CPER_MEM_VALID_REQUESTOR_ID)
		pr_debug("requestor_id: 0x%016llx\n", mem->requestor_id);
	if (mem->validation_bits & CPER_MEM_VALID_RESPONDER_ID)
		pr_debug("responder_id: 0x%016llx\n", mem->responder_id);
	if (mem->validation_bits & CPER_MEM_VALID_TARGET_ID)
		pr_debug("target_id: 0x%016llx\n", mem->target_id);
	if (mem->validation_bits & CPER_MEM_VALID_ERROR_TYPE) {
		u8 etype = mem->error_type;
		printk("%s""error_type: %d, %s\n", pfx, etype,
		       etype < ARRAY_SIZE(cper_mem_err_type_strs) ?
		       cper_mem_err_type_strs[etype] : "unknown");
	}
	if (mem->validation_bits & CPER_MEM_VALID_MODULE_HANDLE) {
		const char *bank = NULL, *device = NULL;
		dmi_memdev_name(mem->mem_dev_handle, &bank, &device);
		if (bank != NULL && device != NULL)
			printk("%s""DIMM location: %s %s", pfx, bank, device);
		else
			printk("%s""DIMM DMI handle: 0x%.4x",
			       pfx, mem->mem_dev_handle);
	}
}

static const char *cper_pcie_port_type_strs[] = {
	"PCIe end point",
	"legacy PCI end point",
	"unknown",
	"unknown",
	"root port",
	"upstream switch port",
	"downstream switch port",
	"PCIe to PCI/PCI-X bridge",
	"PCI/PCI-X to PCIe bridge",
	"root complex integrated endpoint device",
	"root complex event collector",
};

static void cper_print_pcie(const char *pfx, const struct cper_sec_pcie *pcie,
			    const struct acpi_generic_data *gdata)
{
	if (pcie->validation_bits & CPER_PCIE_VALID_PORT_TYPE)
		printk("%s""port_type: %d, %s\n", pfx, pcie->port_type,
		       pcie->port_type < ARRAY_SIZE(cper_pcie_port_type_strs) ?
		       cper_pcie_port_type_strs[pcie->port_type] : "unknown");
	if (pcie->validation_bits & CPER_PCIE_VALID_VERSION)
		printk("%s""version: %d.%d\n", pfx,
		       pcie->version.major, pcie->version.minor);
	if (pcie->validation_bits & CPER_PCIE_VALID_COMMAND_STATUS)
		printk("%s""command: 0x%04x, status: 0x%04x\n", pfx,
		       pcie->command, pcie->status);
	if (pcie->validation_bits & CPER_PCIE_VALID_DEVICE_ID) {
		const __u8 *p;
		printk("%s""device_id: %04x:%02x:%02x.%x\n", pfx,
		       pcie->device_id.segment, pcie->device_id.bus,
		       pcie->device_id.device, pcie->device_id.function);
		printk("%s""slot: %d\n", pfx,
		       pcie->device_id.slot >> CPER_PCIE_SLOT_SHIFT);
		printk("%s""secondary_bus: 0x%02x\n", pfx,
		       pcie->device_id.secondary_bus);
		printk("%s""vendor_id: 0x%04x, device_id: 0x%04x\n", pfx,
		       pcie->device_id.vendor_id, pcie->device_id.device_id);
		p = pcie->device_id.class_code;
		printk("%s""class_code: %02x%02x%02x\n", pfx, p[0], p[1], p[2]);
	}
	if (pcie->validation_bits & CPER_PCIE_VALID_SERIAL_NUMBER)
		printk("%s""serial number: 0x%04x, 0x%04x\n", pfx,
		       pcie->serial_number.lower, pcie->serial_number.upper);
	if (pcie->validation_bits & CPER_PCIE_VALID_BRIDGE_CONTROL_STATUS)
		printk(
	"%s""bridge: secondary_status: 0x%04x, control: 0x%04x\n",
	pfx, pcie->bridge.secondary_status, pcie->bridge.control);

	/* Fatal errors call __ghes_panic() before AER handler prints this */
	if ((pcie->validation_bits & CPER_PCIE_VALID_AER_INFO) &&
	    (gdata->error_severity & CPER_SEV_FATAL)) {
		struct aer_capability_regs *aer;

		aer = (struct aer_capability_regs *)pcie->aer_info;
		printk("%saer_uncor_status: 0x%08x, aer_uncor_mask: 0x%08x\n",
		       pfx, aer->uncor_status, aer->uncor_mask);
		printk("%saer_uncor_severity: 0x%08x\n",
		       pfx, aer->uncor_severity);
		printk("%sTLP Header: %08x %08x %08x %08x\n", pfx,
		       aer->header_log.dw0, aer->header_log.dw1,
		       aer->header_log.dw2, aer->header_log.dw3);
	}
}

static void cper_estatus_print_section(
	const char *pfx, const struct acpi_generic_data *gdata, int sec_no)
{
	uuid_le *sec_type = (uuid_le *)gdata->section_type;
	__u16 severity;
	char newpfx[64];

	severity = gdata->error_severity;
	printk("%s""Error %d, type: %s\n", pfx, sec_no,
	       cper_severity_str(severity));
	if (gdata->validation_bits & CPER_SEC_VALID_FRU_ID)
		printk("%s""fru_id: %pUl\n", pfx, (uuid_le *)gdata->fru_id);
	if (gdata->validation_bits & CPER_SEC_VALID_FRU_TEXT)
		printk("%s""fru_text: %.20s\n", pfx, gdata->fru_text);

	snprintf(newpfx, sizeof(newpfx), "%s%s", pfx, INDENT_SP);
	if (!uuid_le_cmp(*sec_type, CPER_SEC_PROC_GENERIC)) {
		struct cper_sec_proc_generic *proc_err = (void *)(gdata + 1);
		printk("%s""section_type: general processor error\n", newpfx);
		if (gdata->error_data_length >= sizeof(*proc_err))
			cper_print_proc_generic(newpfx, proc_err);
		else
			goto err_section_too_small;
	} else if (!uuid_le_cmp(*sec_type, CPER_SEC_PLATFORM_MEM)) {
		struct cper_sec_mem_err *mem_err = (void *)(gdata + 1);
		printk("%s""section_type: memory error\n", newpfx);
		if (gdata->error_data_length >= sizeof(*mem_err))
			cper_print_mem(newpfx, mem_err);
		else
			goto err_section_too_small;
	} else if (!uuid_le_cmp(*sec_type, CPER_SEC_PCIE)) {
		struct cper_sec_pcie *pcie = (void *)(gdata + 1);
		printk("%s""section_type: PCIe error\n", newpfx);
		if (gdata->error_data_length >= sizeof(*pcie))
			cper_print_pcie(newpfx, pcie, gdata);
		else
			goto err_section_too_small;
	} else
		printk("%s""section type: unknown, %pUl\n", newpfx, sec_type);

	return;

err_section_too_small:
	pr_err(FW_WARN "error section length is too small\n");
}

void cper_estatus_print(const char *pfx,
			const struct acpi_generic_status *estatus)
{
	struct acpi_generic_data *gdata;
	unsigned int data_len, gedata_len;
	int sec_no = 0;
	char newpfx[64];
	__u16 severity;

	severity = estatus->error_severity;
	if (severity == CPER_SEV_CORRECTED)
		printk("%s%s\n", pfx,
		       "It has been corrected by h/w "
		       "and requires no further action");
	printk("%s""event severity: %s\n", pfx, cper_severity_str(severity));
	data_len = estatus->data_length;
	gdata = (struct acpi_generic_data *)(estatus + 1);
	snprintf(newpfx, sizeof(newpfx), "%s%s", pfx, INDENT_SP);
	while (data_len >= sizeof(*gdata)) {
		gedata_len = gdata->error_data_length;
		cper_estatus_print_section(newpfx, gdata, sec_no);
		data_len -= gedata_len + sizeof(*gdata);
		gdata = (void *)(gdata + 1) + gedata_len;
		sec_no++;
	}
}
EXPORT_SYMBOL_GPL(cper_estatus_print);

int cper_estatus_check_header(const struct acpi_generic_status *estatus)
{
	if (estatus->data_length &&
	    estatus->data_length < sizeof(struct acpi_generic_data))
		return -EINVAL;
	if (estatus->raw_data_length &&
	    estatus->raw_data_offset < sizeof(*estatus) + estatus->data_length)
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_GPL(cper_estatus_check_header);

int cper_estatus_check(const struct acpi_generic_status *estatus)
{
	struct acpi_generic_data *gdata;
	unsigned int data_len, gedata_len;
	int rc;

	rc = cper_estatus_check_header(estatus);
	if (rc)
		return rc;
	data_len = estatus->data_length;
	gdata = (struct acpi_generic_data *)(estatus + 1);
	while (data_len >= sizeof(*gdata)) {
		gedata_len = gdata->error_data_length;
		if (gedata_len > data_len - sizeof(*gdata))
			return -EINVAL;
		data_len -= gedata_len + sizeof(*gdata);
		gdata = (void *)(gdata + 1) + gedata_len;
	}
	if (data_len)
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_GPL(cper_estatus_check);