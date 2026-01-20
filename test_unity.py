import pytest
from pytest_embedded import Dut


@pytest.mark.parametrize('target', ['esp32'], indirect=True)
def test_unity_tests_pass(dut: Dut):
    """Verify that all Unity tests pass."""
    # Wait for Unity test output
    dut.expect_exact("=== Running Unity Tests ===", timeout=30)

    # Expect all tests to pass - Unity outputs summary at end
    # Format: "X Tests X Failures X Ignored"
    result = dut.expect(r"(\d+) Tests (\d+) Failures (\d+) Ignored", timeout=60)

    # Extract counts from regex groups
    total_tests = int(result.group(1))
    failures = int(result.group(2))
    ignored = int(result.group(3))

    # Assert no failures
    assert total_tests > 0, "No tests were run"
    assert failures == 0, f"Test failures detected: {failures} out of {total_tests} tests failed"

    # Verify we see the completion message
    dut.expect_exact("=== Unity Tests Complete ===", timeout=10)
