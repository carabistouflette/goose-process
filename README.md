# goose-process

A small synchronous subprocess library for Goose. Apache-2.0.
Experimental API; Linux with glibc 2.34+ is the initial target.

Runs explicit absolute executable paths with an argument array, optional working
directory, separate bounded stdout/stderr captures, and a cancellation deadline.
No implicit shell or PATH search. Native process operations use a small C kernel;
Goose owns validation and returned output. See `CONTRIBUTING.md` for development
and commit standards.

Implementation and API documentation are in progress.
