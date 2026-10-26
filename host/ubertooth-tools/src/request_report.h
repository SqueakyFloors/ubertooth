/*
 * The one report for a control request the board refused, shared by the
 * ubertooth-* host tools that set a capture up.
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
 *     note: if a mode is running, stop it with ubertooth-util -S; if the board does not answer at all, re-plug it
 *
 * <what> is the action the tool asked for, written to follow "could not", for
 * example "set the channel". <code> is the libusb error code the request came
 * back with. A code with no plain words leaves the line as
 * "error: could not <what> (libusb <code>)", and a code with nothing useful to
 * suggest prints no note.
 *
 * This lives apart from the tools so each one names its own request while the
 * wording and the advice stay in one place.
 */
void report_refused_request(const char *what, int code);

#endif /* UBERTOOTH_REQUEST_REPORT_H */
