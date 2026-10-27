MAX_BYTES = 4 * 1024 * 1024


class Unsupported(ValueError):
    """Input cannot be interpreted or edited safely by the selected adapter."""
