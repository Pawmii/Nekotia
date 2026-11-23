namespace Nekotia.Mem;

/// <summary>Page-table statistics. Mirrors <c>neko::vmm::Stats</c>.</summary>
[Native("::neko::vmm::Stats")]
public struct VmStats
{
    [Native("table_pages")] public ulong TablePages;
    [Native("heap_mapped")] public ulong HeapMapped;
    [Native("stacks")] public ulong Stacks;
    [Native("kernel_phys")] public ulong KernelPhysical;
    [Native("text_bytes")] public ulong TextBytes;
    [Native("rodata_bytes")] public ulong RodataBytes;
    [Native("data_bytes")] public ulong DataBytes;
    [Native("nx")] public bool NoExecute;
    [Native("write_combining")] public bool WriteCombining;

    [Native("::neko::vmm::stats")]
    public static extern VmStats Get();
}
