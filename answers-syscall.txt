# Answer to lab/syscall

> Looking at the backtrace output, which function called syscall?

usertrap()

> What is the value of p->trapframe->a7 and what does that value represent? (Hint: look user/initcode.S, the first user program xv6 starts.)

7, which represents the system call number SYS_exec(can be found in `kernel/syscall.h`). In `initcode.S`, `li a7, SYS_exec` means that load 7 into register a7, which is the syscall number for `exec`.

> What was the previous mode that the CPU was in?

User Mode. This can be inferred from the `sstatus` value of `0x200000022`, the `SPP` bit(bit 8) is set to 0, indicating User Mode.

> Write down the assembly instruction the kernel is panicing at. Which register corresponds to the variable num?

The assembly instruction is `lw a3, 0(zero)`.

- `lw`: This is the "load word" instruction. It tells the CPU to read a 4-byte value from memory.
- `a3`: This is the destination register. The value read from memory will be placed into register `a3`.
- `0(zero)`: This specifies the memory address to read from. The zero register always contains the value 0. The instruction adds an offset of 0 to this register, resulting in a final effective address of 0x0.

The register that corresponds to the C variable num is `a3`.

> Why does the kernel crash? Hint: look at figure 3-3 in the text; is address 0 mapped in the kernel address space? Is that confirmed by the value in scause above? (See description of scause in RISC-V privileged instructions)

The kernel crashes because it attempts to execute a **null pointer dereference**.
The instruction `lw a3, 0(zero)` tries to read from memory address 0. This address is intentionally **unmapped** in the kernel's address space to catch such errors.
This is confirmed by the `scause` value of `0xd` (13), which is the specific hardware exception code for a "**Load page fault**", meaning a `load` instruction tried to access an invalid address.

> What is the name of the process that was running when the kernel paniced? What is its process id (pid)?

Based on the GDB output:
- Process Name: `initcode`
- Process ID (PID): `1`