import time
import uuid


class Desktop:
    """AT-SPI adapter. Optional GI dependency; no agent-desktop code/dependency."""

    def __init__(self):
        self.atspi = None
        self.refs = {}
        self.stamp = ""
        self.created = 0

    def load(self):
        if self.atspi is None:
            try:
                import gi
                gi.require_version("Atspi", "2.0")
                from gi.repository import Atspi
                Atspi.set_timeout(1500, 3000)
                self.atspi = Atspi
            except (ImportError, ValueError):
                raise RuntimeError("缺少 AT-SPI Python 支持。Arch: python-gobject at-spi2-core；Ubuntu: python3-gi gir1.2-atspi-2.0。") from None
        return self.atspi

    def snapshot(self, app):
        api = self.load()
        self.refs.clear()
        self.stamp = uuid.uuid4().hex[:12]
        self.created = time.monotonic()
        root = api.get_desktop(0)
        apps = []
        nodes = []
        errors = []
        visited = 0
        deadline = self.created + 8

        def visit(node, depth):
            nonlocal visited
            if visited >= 180 or depth > 10 or time.monotonic() > deadline:
                return
            visited += 1
            try:
                role = node.get_role_name()
                # Never traverse password entry content.
                if node.get_role() == api.Role.PASSWORD_TEXT:
                    return
                name = (node.get_name() or "")[:240]
                state = node.get_state_set()
                actions = []
                action = node.get_action_iface()
                if action:
                    actions = [action.get_action_name(i) for i in range(min(action.get_n_actions(), 16))]
                editable = state.contains(api.StateType.EDITABLE)
                ref = f"{self.stamp}:{len(self.refs) + 1}"
                self.refs[ref] = (node, name, role)
                item = {"ref": ref, "depth": depth, "role": role, "name": name,
                        "actions": actions, "editable": editable,
                        "enabled": state.contains(api.StateType.ENABLED)}
                text = node.get_text_iface()
                if text:
                    item["text"] = text.get_text(0, min(text.get_character_count(), 1200))
                nodes.append(item)
                for i in range(min(node.get_child_count(), 180)):
                    visit(node.get_child_at_index(i), depth + 1)
                    if time.monotonic() > deadline:
                        break
            except Exception:
                errors.append("某控件无法访问，可能已关闭或不支持无障碍接口。")

        for i in range(min(root.get_child_count(), 80)):
            application = root.get_child_at_index(i)
            try:
                name = application.get_name() or ""
                apps.append(name)
                if app and name.casefold() == app.casefold():
                    visit(application, 0)
            except Exception:
                errors.append("某应用无法访问。")
        return {"apps": apps, "app": app, "nodes": nodes,
                "partial": bool(errors) or visited >= 180 or time.monotonic() > deadline,
                "errors": errors[:3],
                "hint": "传入 apps 中的完整应用名读取控件；操作后必须重新 snapshot。"}

    def perform(self, ref, action=None, text=None):
        if time.monotonic() - self.created > 30 or ref not in self.refs:
            raise ValueError("控件引用不存在或已过期，请重新读取桌面。")
        node, name, role = self.refs[ref]
        self.refs.clear()  # One action invalidates all snapshot references.
        if node.get_name() != name or node.get_role_name() != role:
            raise ValueError("控件已变化，请重新读取桌面。")
        api = self.load()
        states = node.get_state_set()
        if not states.contains(api.StateType.ENABLED):
            raise ValueError("控件当前不可用。")
        if text is not None:
            if not states.contains(api.StateType.EDITABLE) or node.get_role() == api.Role.PASSWORD_TEXT:
                raise ValueError("目标不是允许写入的编辑区。")
            interface = node.get_editable_text_iface()
            ok = interface and interface.set_text_contents(text)
        else:
            interface = node.get_action_iface()
            names = [interface.get_action_name(i) for i in range(interface.get_n_actions())] if interface else []
            if action not in names:
                raise ValueError("动作不在该控件公布的动作列表中。")
            ok = interface.do_action(names.index(action))
        return {"accepted": bool(ok), "verified": False,
                "next": "重新读取桌面以检查结果，accepted 不代表任务完成。"}
