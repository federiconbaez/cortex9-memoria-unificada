# Hashes / Historial reescrito

**ES:** El 2026-10-22 se reescribió todo el historial de este repositorio (git-filter-repo) para quitar datos del host y del usuario (nombre de máquina, usuario, uid, rutas de cgroup, versión de kernel). Los commits cambiaron de hash; los documentos de auditoría citan los hashes viejos. Esta tabla da la equivalencia.

**EN:** On 2026-10-22 this repository's full history was rewritten (git-filter-repo) to remove host and user data. Commit hashes changed; audit documents cite the old hashes. This table maps old → new.

**Nota / Note:** el sha256 de `git archive` citado en AUDIT-valor.md (`68b732de…f085`) se calculó sobre el historial previo a la reescritura y no se reproduce con el árbol actual / was computed on the pre-rewrite history and will not reproduce on the current tree.

| Viejo / Old | Nuevo / New | Fecha (UTC) | Mensaje / Message |
|---|---|---|---|
| `7c08313` | `7c08313` | 2026-10-21T02:07:00Z | tiermem: memoria por niveles con userfaultfd, backends pread/io_uring, pruebas, benchmark y documentación |
| `023da41` | `023da41` | 2026-10-21T02:33:58Z | Arreglo: despertar al hilo tras la contabilidad (UFFDIO_COPY_MODE_DONTWAKE + UFFDIO_WAKE); archivos de prueba por PID; verificación bajo carga |
| `b7be730` | `abefa0c` | 2026-10-21T03:35:30Z | Auditoría del experimento de valor del sistema 5: PASA el umbral #5 en aleatorio (B+), secuencial no robusto |
| `f446598` | `1951a57` | 2026-10-22 | Licencia Apache-2.0 (mensaje reescrito con formato nuevo / rewritten message) |
