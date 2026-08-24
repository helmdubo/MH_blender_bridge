"""Stable validation diagnostics shared by bpy host adapters."""

from .canonical import ERROR_CODES

__all__ = [
    "MHValidationError",
    "ValidationError",
    "ValidationWarning",
    "build_report",
]


class MHValidationError(ValueError):
    def __init__(self, code, subjects, message=""):
        assert code in ERROR_CODES, f"unknown error code {code}"
        self.code = code
        self.subjects = sorted(str(subject) for subject in subjects)
        self.message = message
        if len(self.subjects) == 1:
            context = f"subject {self.subjects[0]!r}"
        elif self.subjects:
            context = "subjects [" + ", ".join(
                repr(subject) for subject in self.subjects) + "]"
        else:
            context = "no subject"
        rendered = f"{code}: {context}"
        if message:
            rendered += f": {message}"
        super().__init__(rendered)

    def as_row(self):
        return ValidationError(self.code, self.subjects, str(self))


class ValidationError:
    def __init__(self, code, subjects, message=""):
        assert code in ERROR_CODES, f"unknown error code {code}"
        assert code.startswith("MH_E_")
        self.code = code
        self.subjects = sorted(subjects)
        self.message = message

    def disk_dict(self):
        result = {"code": self.code, "subjects": self.subjects}
        if self.message:
            result["message"] = self.message
        return result


class ValidationWarning(ValidationError):
    def __init__(self, code, subjects, message=""):
        assert code in ERROR_CODES, f"unknown warning code {code}"
        assert code.startswith("MH_W_")
        self.code = code
        self.subjects = sorted(subjects)
        self.message = message


def build_report(errors, warnings=()):
    errors = sorted(errors, key=lambda row: (row.code, row.subjects))
    warnings = sorted(warnings, key=lambda row: (row.code, row.subjects))
    return {
        "schema": "mh.validation_report",
        "schema_version": 2,
        "errors": [row.disk_dict() for row in errors],
        "warnings": [row.disk_dict() for row in warnings],
    }
