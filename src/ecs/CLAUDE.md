# ecs/

Generational `Entity` handles; one sparse-set `ComponentStorage<T>` per type in `World`. Systems are free functions over `world.each<A, B>(...)`.

- List the rarest component first in `each`.
- Inside `each`, `destroy` and `remove` are queued until the outermost walk ends.
- Adding a component of a walked type inside `each` throws; use `addLater`.
- A new component that ticks change goes into `hashSimulationState` (`engine/state_hash`).
- Not built yet: parent/child hierarchies (`ROADMAP.md` phase 9).
