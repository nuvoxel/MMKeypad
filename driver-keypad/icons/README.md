# Driver icons

Composer expects two PNG device icons referenced by `driver.xml`:

- `device_sm.png` — small icon (32×32)
- `device_lg.png` — large icon (~100×100 / 128×128)

These are binary assets — drop them in here before packaging. Until then the
driver loads fine but shows a default/blank icon in Composer.
