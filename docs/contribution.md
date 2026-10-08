# Contributing to mnet

Thank you for your interest in contributing to mnet! This document describes how
to contribute to the project.

## How to contribute

1. Fork the repository on GitHub.
2. Create a branch for your feature or bugfix.
3. Make your changes.
4. Ensure all tests pass (`make test`).
5. Submit a pull request.

## Code style

- C17 standard.
- `-Wall -Wextra -Wpedantic -Werror` must compile cleanly.
- Use `mnet_*` prefix for public API functions.
- Use `MNET_*` prefix for public macros.
- All allocation must be checked for failure.
- All memory must be freed after use.

## Testing

All contributions must include tests. Run the test suite with:

```sh
make test
```

All tests must pass.

## Security

mnet is a security-sensitive project. All contributions must be reviewed for
security issues. The following must be tested:

- HTTP request parsing (headers, body, query string, cookies)
- Path traversal attacks
- Header injection / response splitting
- Memory corruption (buffer overflows, use-after-free, double-free)
- Information disclosure (path traversal, exposure of sensitive data)
- Denial of service (slowloris, oversized requests)

## Commit messages

Use the following format:

```
<type>: <subject>

<body>

<footer>
```

Types:

- `feat`: A new feature
- `fix`: A bug fix
- `docs`: Documentation only changes
- `test`: Adding missing tests or correcting existing tests
- `refactor`: A code change that neither fixes a bug nor adds a feature
- `perf`: A code change that improves performance
- `chore`: Changes to the build process or auxiliary tools

## Pull request checklist

- [ ] Code compiles with `-Wall -Wextra -Wpedantic -Werror`
- [ ] All tests pass (`make test`)
- [ ] New features include tests
- [ ] New features include documentation
- [ ] No memory leaks or buffer overflows
- [ ] No security vulnerabilities
- [ ] Commit messages follow the format described above

## Questions

If you have questions, please open an issue on GitHub.
