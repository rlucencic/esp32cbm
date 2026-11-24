# esp32cbm roadmap

Proposed work in priority order. See [Development](README.md#development) for the current project.

3. **Check 40- and 80-column use on the Tab5.** Use a physical keyboard in C128
   BASIC and CP/M, switch the displayed output, and check a 40-column SID tone
   on the speaker. Record which keyboard paths were exercised and verify that
   changing output leaves the mounted disk and guest session intact.

4. **Trace the Hessian IEC fast-loader stall.** Reproduce the default load
   with the D64 and G64 fixtures in the [media validation report](docs/media-validation-2026-11-24.md).
   Compare the first diverging CIA/VIA serial transitions with a reference
   1541 trace. Check the fallback and a normal KERNAL load after any fix.
