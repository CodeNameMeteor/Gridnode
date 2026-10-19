# db/annotations

Hand-written findings about the engine that reflection does not provide: runtime class members,
vtable slots, function signatures, globals. One YAML file per class or subsystem, grouped by
Frostbite module. The schema and the first annotations (everything MECMP relies on) arrive in M2;
see [docs/PLAN.md](../../docs/PLAN.md) §4.1.

Annotations name reflected types but never copy their field lists, so this folder holds only our
own reversing results.
