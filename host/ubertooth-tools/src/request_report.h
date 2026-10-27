/*
 * The reports for a capture that could not be started, shared by the
 * ubertooth-* host tools that set one up: a control request the board
 * refused, and the host thread that collects the capture.
 *
 * This file is part of Project Ubertooth.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifndef UBERTOOTH_REQUEST_REPORT_H
#define UBERTOOTH_REQUEST_REPORT_H

/*
 * Prints, on standard error, one line for a request the board refused and at
 * most one note on what to try next:
 *
 *     error: could not set the channel: the board did not answer in time (libusb -7)
 *     note: if a mode is running, stop it with ubertooth-util -S; a board that does not answer at all usually resets itself within a few seconds, so run the command again, and unplug it or power-cycle its hub port if it stays silent
 *
 * <what> is the action the tool asked for, written to follow "could not", for
 * example "set the channel". <code> is the libusb error code the request came
 * back with. A code with no plain words leaves the line as
 * "error: could not <what> (libusb <code>)", and a code with nothing useful to
 * suggest prints no note.
 *
 * Prints nothing for UBERTOOTH_ERROR_SHORT_REPLY, whose whole line
 * libubertooth has already printed.
 *
 * This lives apart from the tools so each one names its own request while the
 * wording and the advice stay in one place.
 */
void report_refused_request(const char *what, int code);

/*
 * Prints, on standard error, the one line for a capture whose host-side
 * thread could not be started:
 *
 *     error: could not start the thread that collects the capture: Resource temporarily unavailable
 *
 * <error> is the errno value pthread_create() returned. That is where this
 * differs from the report above: a thread that cannot be created is a host
 * failure, with no libusb code and nothing the board did wrong.
 */
void report_capture_thread_failure(int error);

#endif /* UBERTOOTH_REQUEST_REPORT_H */
