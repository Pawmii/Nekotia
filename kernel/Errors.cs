using System;

namespace Nekotia.OS;

/// Kernel-specific exception. Exception classes are allowed as `throw` targets
/// (nekoc does not emit them): an uncaught throw shows the stop screen with this type name.
public class KernelError : Exception
{
    public KernelError(string message) : base(message) { }
}
