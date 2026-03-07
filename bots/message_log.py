import sys
from collections import deque
from io import StringIO


class MessageLog:
    def __init__(self, max_lines: int = 1000):
        self.messages = deque(maxlen=max_lines)
        self._original_stdout = sys.stdout
        self._original_stderr = sys.stderr
        self._capture = StringIO()
        
    def start_capture(self) -> None:
        """Start capturing print() output."""
        sys.stdout = self
        sys.stderr = self
        
    def stop_capture(self) -> None:
        """Stop capturing and restore original stdout/stderr."""
        sys.stdout = self._original_stdout
        sys.stderr = self._original_stderr
        
    def write(self, text: str) -> None:
        """Capture text from print() and also write to original stdout."""
        self._original_stdout.write(text)
        if text.strip():
            self.messages.append(text.rstrip('\n'))
            
    def flush(self) -> None:
        """Required for file-like objects."""
        self._original_stdout.flush()
        
    def get_messages(self, max_count: int = 100) -> list[str]:
        """Get recent messages for display."""
        return list(self.messages)[-max_count:]
        
    def clear(self) -> None:
        """Clear all messages."""
        self.messages.clear()


# Global instance
message_log = MessageLog()
