/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GDB_SERVER_H
#define GDB_SERVER_H

/*
 * Body of the SWD owner task. Initialises the BMD platform layer and runs the GDB server
 * forever. Must be called from a task pinned to the core that owns SWD.
 */
void probe_task_main(void);

#endif /* GDB_SERVER_H */
