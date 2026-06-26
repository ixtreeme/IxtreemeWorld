// Stage-6 Animator node-graph editor panel (read-only). Renders the AnimatorGraphEditorState
// snapshot (pushed by EngineApplication) onto an ImDrawList canvas: grid, state nodes, transition
// edges, pan/zoom, click-to-select. ONE InvisibleButton + manual hit-test (nesting per-node items
// causes active-id contention — the lesson the Scene View already paid for). Editing is Stage 7.
//
// Included from EditorImGui.cpp inside the editor-enabled implementation block.

void EditorImGui::RenderAnimatorPanel()
{
    if (!m_animatorPanelOpen)
        return;

    if (!ImGui::Begin(ICON_FA_PERSON_RUNNING " Animator", &m_animatorPanelOpen,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        m_animatorGraphVisible = false;
        ImGui::End();
        return;
    }
    m_animatorGraphVisible = true;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 availSize = ImGui::GetContentRegionAvail();
    // Reserve a LEFT column for the Parameters panel and a BOTTOM strip for the selected state /
    // transition editor. Keeping every widget OUT of the canvas rectangle — never overlapping the
    // single canvas InvisibleButton — avoids the ImGui active-id contention the Scene View paid for.
    const float paramPanelW = m_animatorGraphState.hasController ? 196.0f : 0.0f;
    const bool showTransitionEditor = m_animatorGraphState.hasController && m_animatorEditEdgeValid;
    const bool showStateInspector = m_animatorGraphState.hasController && !showTransitionEditor &&
        m_animatorSelectedStateId != 0u && m_animatorSelectedStateId != 0xFFFFFFFFu;
    int selStateMotionType = 0;  // 0 Single, 1 1D, 2 2D — a blend-tree state needs a taller strip
    if (showStateInspector)
        for (const AnimatorGraphNode& n : m_animatorGraphState.nodes)
            if (n.id == m_animatorSelectedStateId) { selStateMotionType = n.blendTreeType; break; }
    const float stateStripH = (selStateMotionType != 0) ? 244.0f : 132.0f;
    const float inspectorStripH = showTransitionEditor ? 210.0f : (showStateInspector ? stateStripH : 0.0f);
    const ImVec2 canvasMin(origin.x + paramPanelW, origin.y);
    const ImVec2 canvasSize(availSize.x - paramPanelW, availSize.y - inspectorStripH);
    const ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();

    if (canvasSize.x < 4.0f || canvasSize.y < 4.0f)
    {
        ImGui::End();
        return;
    }

    dl->AddRectFilled(canvasMin, canvasMax, IM_COL32(28, 30, 36, 255));

    if (!m_animatorGraphState.hasController)
    {
        const char* msg = "Select a mesh entity with an Animator Controller.";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(canvasMin.x + (canvasSize.x - ts.x) * 0.5f, canvasMin.y + (canvasSize.y - ts.y) * 0.5f),
            IM_COL32(150, 156, 168, 255), msg);
        ImGui::End();
        return;
    }

    // Center the view on the graph ONCE, when a different controller first appears. Node positions
    // are raw controller coordinates; g2s maps the centroid to the canvas center at this pan. After
    // this the user pans/zooms freely and editing a node never shifts the rest of the graph.
    if (m_animatorGraphState.controllerId != m_animatorCenteredControllerId &&
        !m_animatorGraphState.nodes.empty())
    {
        m_animatorCenteredControllerId = m_animatorGraphState.controllerId;
        float minX = m_animatorGraphState.nodes.front().graphPos[0];
        float minY = m_animatorGraphState.nodes.front().graphPos[1];
        float maxX = minX + 150.0f;
        float maxY = minY + 46.0f;
        for (const AnimatorGraphNode& n : m_animatorGraphState.nodes)
        {
            minX = std::min(minX, n.graphPos[0]);
            minY = std::min(minY, n.graphPos[1]);
            maxX = std::max(maxX, n.graphPos[0] + 150.0f);
            maxY = std::max(maxY, n.graphPos[1] + 46.0f);
        }
        m_animatorPan[0] = -(minX + maxX) * 0.5f;
        m_animatorPan[1] = -(minY + maxY) * 0.5f;
        m_animatorZoom = 1.0f;
    }

    const ImVec2 center(canvasMin.x + canvasSize.x * 0.5f, canvasMin.y + canvasSize.y * 0.5f);
    const float zoom = m_animatorZoom;
    auto g2s = [&](float gx, float gy) -> ImVec2 {
        return ImVec2(center.x + (gx + m_animatorPan[0]) * zoom, center.y + (gy + m_animatorPan[1]) * zoom);
    };
    auto s2g = [&](const ImVec2& s) -> ImVec2 {
        return ImVec2((s.x - center.x) / zoom - m_animatorPan[0], (s.y - center.y) / zoom - m_animatorPan[1]);
    };
    const float nodeW = 150.0f * zoom;
    const float nodeH = 46.0f * zoom;

    // Stage 7 interaction state (persists across frames within this single panel instance).
    constexpr std::uint32_t kAnyStateSentinel = 0xFFFFFFFFu;
    static bool s_pressedEmpty = false;
    static bool s_draggingNode = false;
    static std::uint32_t s_dragNodeId = 0;
    static float s_dragAccumX = 0.0f;
    static float s_dragAccumY = 0.0f;
    static ImVec2 s_leftPressPos(0.0f, 0.0f);
    // Screen-space edge midpoints (filled by the edge draw loop, hit-tested next frame for
    // left-click edge-select and right-click edge-delete).
    struct EdgeMid { ImVec2 mid; std::uint32_t from; std::uint32_t to; };
    static std::vector<EdgeMid> s_edgeMidpoints;

    // Node top-left in screen space; the actively-dragged node follows the cursor by the
    // accumulated (uncommitted) delta until the MoveNode edit lands on mouse-release.
    auto nodeMin = [&](const AnimatorGraphNode& n) -> ImVec2 {
        float gx = n.graphPos[0];
        float gy = n.graphPos[1];
        if (s_draggingNode && n.id == s_dragNodeId)
        {
            gx += s_dragAccumX;
            gy += s_dragAccumY;
        }
        return g2s(gx, gy);
    };

    // --- input: single full-canvas item, manual hit-test ---
    ImGui::SetCursorScreenPos(canvasMin);
    ImGui::InvisibleButton("##animgraph_canvas", canvasSize);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    if (hovered && io.MouseWheel != 0.0f)
    {
        const ImVec2 beforeG = s2g(io.MousePos);
        m_animatorZoom = std::clamp(m_animatorZoom * (io.MouseWheel > 0.0f ? 1.1f : 1.0f / 1.1f), 0.25f, 3.0f);
        // recompute the cursor's graph point at the new zoom and shift pan so it stays fixed
        const float nz = m_animatorZoom;
        const ImVec2 afterG((io.MousePos.x - center.x) / nz - m_animatorPan[0],
                            (io.MousePos.y - center.y) / nz - m_animatorPan[1]);
        m_animatorPan[0] += afterG.x - beforeG.x;
        m_animatorPan[1] += afterG.y - beforeG.y;
    }

    if (ImGui::IsItemActivated())
    {
        // topmost-first hit-test against screen-space node AABBs
        std::uint32_t hit = 0;
        bool found = false;
        for (auto it = m_animatorGraphState.nodes.rbegin(); it != m_animatorGraphState.nodes.rend(); ++it)
        {
            const ImVec2 mn = nodeMin(*it);
            const ImVec2 mx(mn.x + nodeW, mn.y + nodeH);
            if (io.MousePos.x >= mn.x && io.MousePos.x <= mx.x && io.MousePos.y >= mn.y && io.MousePos.y <= mx.y)
            {
                hit = it->id;
                found = true;
                break;
            }
        }
        m_animatorSelectedStateId = found ? hit : 0;
        if (found)
            m_animatorEditEdgeValid = false;  // selecting a state deselects any edge being edited
        s_pressedEmpty = !found;
        s_leftPressPos = io.MousePos;
        // Real (non-synthetic) state nodes are draggable; Entry/Any-State and empty space are not.
        s_draggingNode = found && hit != 0u && hit != kAnyStateSentinel;
        s_dragNodeId = s_draggingNode ? hit : 0u;
        s_dragAccumX = 0.0f;
        s_dragAccumY = 0.0f;
    }
    if (active && s_draggingNode && zoom > 0.0001f)
    {
        s_dragAccumX += io.MouseDelta.x / zoom;
        s_dragAccumY += io.MouseDelta.y / zoom;
    }
    else if (active && s_pressedEmpty && zoom > 0.0001f)
    {
        m_animatorPan[0] += io.MouseDelta.x / zoom;
        m_animatorPan[1] += io.MouseDelta.y / zoom;
    }
    if (ImGui::IsItemDeactivated() && s_draggingNode)
    {
        // Commit the drag as a single centroid-invariant MoveNode delta (the snapshot is re-centered
        // every frame, so an absolute position would drift).
        if (s_dragAccumX != 0.0f || s_dragAccumY != 0.0f)
        {
            AnimatorGraphEdit moveEdit;
            moveEdit.type = AnimatorGraphEditType::MoveNode;
            moveEdit.stateId = s_dragNodeId;
            moveEdit.graphDeltaX = s_dragAccumX;
            moveEdit.graphDeltaY = s_dragAccumY;
            m_commands.animatorEdits.push_back(std::move(moveEdit));
        }
        s_draggingNode = false;
        s_dragNodeId = 0u;
        s_dragAccumX = 0.0f;
        s_dragAccumY = 0.0f;
    }
    // Left-click (no node, no pan-drag) on an edge midpoint selects that transition for editing.
    if (ImGui::IsItemDeactivated() && s_pressedEmpty && !s_draggingNode)
    {
        const float mvx = io.MousePos.x - s_leftPressPos.x;
        const float mvy = io.MousePos.y - s_leftPressPos.y;
        if (mvx * mvx + mvy * mvy < 36.0f)  // a click, not a pan
        {
            bool edgeHit = false;
            for (const EdgeMid& em : s_edgeMidpoints)
            {
                const float dx = io.MousePos.x - em.mid.x;
                const float dy = io.MousePos.y - em.mid.y;
                if (dx * dx + dy * dy < 100.0f)  // 10px
                {
                    m_animatorEditEdgeValid = true;
                    m_animatorEditEdgeFrom = em.from;
                    m_animatorEditEdgeTo = em.to;
                    m_animatorEditSeededFrom = 0xFFFFFFFEu;  // force a fresh seed from the snapshot
                    m_animatorSelectedStateId = 0;  // editing an edge deselects the state
                    edgeHit = true;
                    break;
                }
            }
            if (!edgeHit)
                m_animatorEditEdgeValid = false;  // clicked empty space -> deselect the edge
        }
    }

    // --- right mouse: drag node->node = create transition; quick-click = context menu;
    //     click on an edge = delete; click on empty space = add-state menu ---
    static float s_ctxAddX = 0.0f;
    static float s_ctxAddY = 0.0f;
    static std::uint32_t s_rmbFrom = 0;       // node the RMB went down on (real state or Any-State)
    static bool s_rmbOnNode = false;
    static bool s_rmbDragging = false;
    static ImVec2 s_rmbDownPos(0.0f, 0.0f);
    static std::uint32_t s_edgeDelFrom = 0;
    static std::uint32_t s_edgeDelTo = 0;

    auto hitNodeAtCursor = [&](std::uint32_t& outId) -> bool {
        for (auto it = m_animatorGraphState.nodes.rbegin(); it != m_animatorGraphState.nodes.rend(); ++it)
        {
            const ImVec2 mn = nodeMin(*it);
            const ImVec2 mx(mn.x + nodeW, mn.y + nodeH);
            if (io.MousePos.x >= mn.x && io.MousePos.x <= mx.x && io.MousePos.y >= mn.y && io.MousePos.y <= mx.y)
            {
                outId = it->id;
                return true;
            }
        }
        return false;
    };

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        std::uint32_t hit = 0;
        const bool found = hitNodeAtCursor(hit);
        s_rmbDownPos = io.MousePos;
        s_rmbDragging = false;
        s_rmbOnNode = found && hit != 0u;  // a real state or the Any-State node (not Entry id 0)
        s_rmbFrom = s_rmbOnNode ? hit : 0u;
    }
    if (s_rmbOnNode && ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        const float dx = io.MousePos.x - s_rmbDownPos.x;
        const float dy = io.MousePos.y - s_rmbDownPos.y;
        if (dx * dx + dy * dy > 36.0f)  // 6px drag threshold
            s_rmbDragging = true;
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
    {
        std::uint32_t target = 0;
        const bool overNode = hitNodeAtCursor(target);
        if (s_rmbOnNode && s_rmbDragging)
        {
            // dragged from a node: create a transition to a distinct real target state
            if (overNode && target != 0u && target != kAnyStateSentinel && target != s_rmbFrom)
            {
                AnimatorGraphEdit e;
                e.type = AnimatorGraphEditType::CreateTransition;
                e.stateId = s_rmbFrom;     // from (kAnyStateSentinel == Any-State)
                e.toStateId = target;
                m_commands.animatorEdits.push_back(std::move(e));
            }
        }
        else if (s_rmbOnNode && !s_rmbDragging && s_rmbFrom != kAnyStateSentinel)
        {
            // quick right-click on a real state -> node context menu
            m_animatorSelectedStateId = s_rmbFrom;
            ImGui::OpenPopup("##anim_node_ctx");
        }
        else if (!s_rmbOnNode && hovered)
        {
            // empty area: prefer an edge under the cursor (delete), else the add-state menu
            bool edgeHit = false;
            for (const EdgeMid& em : s_edgeMidpoints)
            {
                const float dx = io.MousePos.x - em.mid.x;
                const float dy = io.MousePos.y - em.mid.y;
                if (dx * dx + dy * dy < 100.0f)  // 10px
                {
                    s_edgeDelFrom = em.from;
                    s_edgeDelTo = em.to;
                    edgeHit = true;
                    break;
                }
            }
            if (edgeHit)
            {
                ImGui::OpenPopup("##anim_edge_ctx");
            }
            else
            {
                const ImVec2 g = s2g(s_rmbDownPos);
                s_ctxAddX = g.x;
                s_ctxAddY = g.y;
                ImGui::OpenPopup("##anim_canvas_ctx");
            }
        }
        s_rmbOnNode = false;
        s_rmbFrom = 0u;
        s_rmbDragging = false;
    }
    if (ImGui::BeginPopup("##anim_edge_ctx"))
    {
        if (ImGui::MenuItem("Delete Transition"))
        {
            AnimatorGraphEdit e;
            e.type = AnimatorGraphEditType::DeleteTransition;
            e.stateId = s_edgeDelFrom;
            e.toStateId = s_edgeDelTo;
            m_commands.animatorEdits.push_back(std::move(e));
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##anim_node_ctx"))
    {
        if (ImGui::MenuItem("Set as Default"))
        {
            AnimatorGraphEdit e;
            e.type = AnimatorGraphEditType::SetDefaultState;
            e.stateId = m_animatorSelectedStateId;
            m_commands.animatorEdits.push_back(std::move(e));
        }
        if (ImGui::MenuItem("Rename..."))
        {
            m_animatorRenameStateId = m_animatorSelectedStateId;
            std::size_t i = 0;
            for (const AnimatorGraphNode& n : m_animatorGraphState.nodes)
                if (n.id == m_animatorSelectedStateId)
                {
                    for (; i + 1 < sizeof(m_animatorRenameBuf) && i < n.name.size(); ++i)
                        m_animatorRenameBuf[i] = n.name[i];
                    break;
                }
            m_animatorRenameBuf[i] = '\0';
            m_animatorRenameRequested = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete"))
        {
            AnimatorGraphEdit e;
            e.type = AnimatorGraphEditType::DeleteState;
            e.stateId = m_animatorSelectedStateId;
            m_commands.animatorEdits.push_back(std::move(e));
            m_animatorSelectedStateId = 0;
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##anim_canvas_ctx"))
    {
        if (ImGui::MenuItem("Add State Here"))
        {
            AnimatorGraphEdit e;
            e.type = AnimatorGraphEditType::AddState;
            e.graphDeltaX = s_ctxAddX;
            e.graphDeltaY = s_ctxAddY;
            m_commands.animatorEdits.push_back(std::move(e));
        }
        ImGui::EndPopup();
    }
    if (m_animatorRenameRequested)
    {
        ImGui::OpenPopup("Rename State##anim");
        m_animatorRenameRequested = false;
    }
    if (ImGui::BeginPopupModal("Rename State##anim", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("##anim_rename", m_animatorRenameBuf, sizeof(m_animatorRenameBuf));
        if (ImGui::Button("OK", ImVec2(80.0f, 0.0f)))
        {
            AnimatorGraphEdit e;
            e.type = AnimatorGraphEditType::RenameState;
            e.stateId = m_animatorRenameStateId;
            e.text = m_animatorRenameBuf;
            m_commands.animatorEdits.push_back(std::move(e));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    dl->PushClipRect(canvasMin, canvasMax, true);

    // --- grid ---
    const float step = 32.0f * zoom;
    if (step >= 6.0f)
    {
        const float ox = std::fmod((m_animatorPan[0] * zoom + canvasSize.x * 0.5f), step);
        for (float x = canvasMin.x + ox; x < canvasMax.x; x += step)
            dl->AddLine(ImVec2(x, canvasMin.y), ImVec2(x, canvasMax.y), IM_COL32(40, 43, 50, 255));
        const float oy = std::fmod((m_animatorPan[1] * zoom + canvasSize.y * 0.5f), step);
        for (float y = canvasMin.y + oy; y < canvasMax.y; y += step)
            dl->AddLine(ImVec2(canvasMin.x, y), ImVec2(canvasMax.x, y), IM_COL32(40, 43, 50, 255));
    }

    // node id -> screen rect, for edge endpoints + dedup
    std::unordered_map<std::uint32_t, std::pair<ImVec2, ImVec2>> rectById;
    rectById.reserve(m_animatorGraphState.nodes.size());
    for (const AnimatorGraphNode& n : m_animatorGraphState.nodes)
    {
        const ImVec2 mn = nodeMin(n);
        rectById[n.id] = {mn, ImVec2(mn.x + nodeW, mn.y + nodeH)};
    }

    // --- edges (under nodes) ---
    s_edgeMidpoints.clear();
    std::unordered_map<std::uint64_t, int> pairIndex;  // dedup parallel edges for offset
    for (const AnimatorGraphEdge& e : m_animatorGraphState.edges)
    {
        auto fromIt = rectById.find(e.fromStateId);
        auto toIt = rectById.find(e.toStateId);
        if (fromIt == rectById.end() || toIt == rectById.end())
            continue;
        const std::uint64_t key = (static_cast<std::uint64_t>(std::min(e.fromStateId, e.toStateId)) << 32) |
                                  std::max(e.fromStateId, e.toStateId);
        const float off = static_cast<float>(pairIndex[key]++) * 9.0f * zoom;

        const ImVec2& fmn = fromIt->second.first;
        const ImVec2& fmx = fromIt->second.second;
        const ImVec2& tmn = toIt->second.first;
        const ImVec2& tmx = toIt->second.second;
        ImVec2 a(fmx.x, (fmn.y + fmx.y) * 0.5f + off);
        ImVec2 b(tmn.x, (tmn.y + tmx.y) * 0.5f + off);
        const ImVec2 c1(a.x + 60.0f * zoom, a.y);
        const ImVec2 c2(b.x - 60.0f * zoom, b.y);

        const ImU32 col = e.active ? IM_COL32(120, 200, 120, 255)
                          : e.isAnyState ? IM_COL32(90, 200, 200, 255)
                                         : IM_COL32(150, 150, 160, 255);
        dl->AddBezierCubic(a, c1, c2, b, IM_COL32(15, 15, 18, 200), 4.0f);
        dl->AddBezierCubic(a, c1, c2, b, col, 2.0f);

        // arrowhead at b along the c2->b tangent
        float tx = b.x - c2.x, ty = b.y - c2.y;
        const float tl = std::sqrt(tx * tx + ty * ty);
        if (tl > 0.0001f)
        {
            tx /= tl;
            ty /= tl;
            const float ah = 10.0f * zoom, aw = 5.0f * zoom;
            const ImVec2 base(b.x - tx * ah, b.y - ty * ah);
            dl->AddTriangleFilled(b, ImVec2(base.x - ty * aw, base.y + tx * aw),
                ImVec2(base.x + ty * aw, base.y - tx * aw), col);
        }
        const ImVec2 mid(0.125f * a.x + 0.375f * c1.x + 0.375f * c2.x + 0.125f * b.x,
                         0.125f * a.y + 0.375f * c1.y + 0.375f * c2.y + 0.125f * b.y);
        if (e.conditionCount > 0 && zoom >= 0.5f)
            dl->AddCircleFilled(mid, 3.0f * zoom, IM_COL32(220, 200, 120, 255));
        // record the midpoint for right-click edge deletion (skip the synthetic Entry edge)
        if (e.fromStateId != 0u)
            s_edgeMidpoints.push_back({mid, e.fromStateId, e.toStateId});
    }

    // --- nodes ---
    const bool drawLabels = zoom >= 0.6f;
    for (const AnimatorGraphNode& n : m_animatorGraphState.nodes)
    {
        const ImVec2 mn = nodeMin(n);
        const ImVec2 mx(mn.x + nodeW, mn.y + nodeH);
        const ImU32 body = n.isActive ? IM_COL32(54, 86, 54, 255)
                           : n.isAnyState ? IM_COL32(40, 78, 82, 255)
                           : n.isEntry ? IM_COL32(70, 60, 40, 255)
                           : n.isDefault ? IM_COL32(58, 64, 86, 255)
                                         : IM_COL32(48, 52, 62, 255);
        dl->AddRectFilled(mn, mx, body, 6.0f);
        dl->AddRectFilled(mn, ImVec2(mx.x, mn.y + 18.0f * zoom), IM_COL32(0, 0, 0, 60), 6.0f);

        const bool selected = (n.id == m_animatorSelectedStateId) && m_animatorSelectedStateId != 0;
        dl->AddRect(mn, mx, selected ? IM_COL32(255, 210, 92, 255) : IM_COL32(70, 76, 90, 255),
            6.0f, 0, selected ? 3.0f : 1.5f);

        if (drawLabels)
        {
            dl->AddText(ImVec2(mn.x + 8.0f * zoom, mn.y + 3.0f * zoom), IM_COL32(235, 238, 244, 255),
                n.name.c_str(), nullptr);
            if (!n.clipLabel.empty())
                dl->AddText(ImVec2(mn.x + 8.0f * zoom, mn.y + 24.0f * zoom),
                    IM_COL32(170, 176, 188, 255), n.clipLabel.c_str(), nullptr);
        }
    }

    // live transition-drag preview (right-drag from a node to the cursor)
    if (s_rmbOnNode && s_rmbDragging)
    {
        auto fromIt = rectById.find(s_rmbFrom);
        if (fromIt != rectById.end())
        {
            const ImVec2& fmn = fromIt->second.first;
            const ImVec2& fmx = fromIt->second.second;
            const ImVec2 a(fmx.x, (fmn.y + fmx.y) * 0.5f);
            const ImVec2 b = io.MousePos;
            const ImVec2 c1(a.x + 60.0f * zoom, a.y);
            const ImVec2 c2(b.x - 60.0f * zoom, b.y);
            dl->AddBezierCubic(a, c1, c2, b, IM_COL32(255, 210, 92, 220), 2.0f);
            dl->AddCircleFilled(b, 3.0f * zoom, IM_COL32(255, 210, 92, 220));
        }
    }

    dl->PopClipRect();

    // footer hint
    dl->AddText(ImVec2(canvasMin.x + 8.0f, canvasMax.y - 20.0f), IM_COL32(120, 126, 138, 255),
        m_animatorGraphState.controllerDisplayName.c_str());

    // --- Stage 7: selected-state inspector strip (rendered BELOW the canvas, never overlapping) ---
    if (showStateInspector && m_assetLibrary)
    {
        const AnimatorGraphNode* selNode = nullptr;
        for (const AnimatorGraphNode& n : m_animatorGraphState.nodes)
            if (n.id == m_animatorSelectedStateId) { selNode = &n; break; }
        if (selNode != nullptr && !selNode->isEntry && !selNode->isAnyState)
        {
            ImGui::SetCursorScreenPos(ImVec2(canvasMin.x + 6.0f, canvasMax.y + 4.0f));
            ImGui::BeginChild("##anim_state_inspector", ImVec2(canvasSize.x - 12.0f, inspectorStripH - 8.0f), true);
            ImGui::TextUnformatted(selNode->name.c_str());

            const std::vector<AssetLibrary::Entry> clips =
                m_assetLibrary->EntriesFor(AssetLibrary::Category::AnimationClip);
            // Renders a clip combo; on a pick, writes the chosen clip id (empty = "(no clip)") into
            // outClip and returns true.
            auto clipCombo = [&](const char* id, float width, const std::string& current,
                                 std::string& outClip) -> bool {
                std::string preview = "(no clip)";
                for (const AssetLibrary::Entry& e : clips)
                    if (e.id == current) { preview = e.displayName; break; }
                bool changed = false;
                ImGui::SetNextItemWidth(width);
                if (ImGui::BeginCombo(id, preview.c_str()))
                {
                    if (ImGui::Selectable("(no clip)", current.empty())) { outClip.clear(); changed = true; }
                    for (const AssetLibrary::Entry& entry : clips)
                    {
                        const bool isSel = (entry.id == current);
                        if (ImGui::Selectable(entry.displayName.c_str(), isSel)) { outClip = entry.id; changed = true; }
                        if (isSel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                return changed;
            };

            // Motion type selector (2D editing arrives in Chunk 4).
            const char* motionNames[] = {"Single", "1D Blend", "2D Blend"};
            const int motionIdx = std::clamp(selNode->blendTreeType, 0, 2);
            ImGui::TextDisabled("Motion");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::BeginCombo("##anim_motion", motionNames[motionIdx]))
            {
                for (int m = 0; m < 3; ++m)
                    if (ImGui::Selectable(motionNames[m], m == motionIdx))
                    {
                        AnimatorGraphEdit e;
                        e.type = AnimatorGraphEditType::SetStateMotionType;
                        e.stateId = m_animatorSelectedStateId;
                        e.text2 = (m == 1) ? "1d" : (m == 2) ? "2d" : "single";
                        m_commands.animatorEdits.push_back(std::move(e));
                    }
                ImGui::EndCombo();
            }

            if (motionIdx == 0)  // single clip
            {
                ImGui::TextDisabled("Clip");
                ImGui::SameLine();
                std::string newClip;
                if (clipCombo("##anim_state_clip", 280.0f, selNode->clipId, newClip))
                {
                    AnimatorGraphEdit clipEdit;
                    clipEdit.type = AnimatorGraphEditType::AssignClip;
                    clipEdit.stateId = m_animatorSelectedStateId;
                    clipEdit.text = newClip;
                    m_commands.animatorEdits.push_back(std::move(clipEdit));
                }
            }
            else if (motionIdx == 1 || motionIdx == 2)  // 1D / 2D blend tree
            {
                const bool is2D = (motionIdx == 2);

                auto blendParamCombo = [&](const char* id, const std::string& current,
                                           AnimatorGraphEditType editType) {
                    ImGui::SetNextItemWidth(120.0f);
                    if (ImGui::BeginCombo(id, current.empty() ? "(param)" : current.c_str()))
                    {
                        for (const AnimatorGraphParameter& p : m_animatorGraphState.parameters)
                            if (p.type == AnimEditParamType::Float || p.type == AnimEditParamType::Int)
                                if (ImGui::Selectable(p.name.c_str(), p.name == current))
                                {
                                    AnimatorGraphEdit e;
                                    e.type = editType;
                                    e.stateId = m_animatorSelectedStateId;
                                    e.text = p.name;
                                    m_commands.animatorEdits.push_back(std::move(e));
                                }
                        ImGui::EndCombo();
                    }
                };

                ImGui::TextDisabled(is2D ? "Param X" : "Param");
                ImGui::SameLine();
                blendParamCombo("##anim_blend_param", selNode->blendParam,
                    AnimatorGraphEditType::SetStateBlendParam);
                if (is2D)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Y");
                    ImGui::SameLine();
                    blendParamCombo("##anim_blend_paramy", selNode->blendParamY,
                        AnimatorGraphEditType::SetStateBlendParamY);
                }

                ImGui::TextDisabled(is2D ? "Motions (clip @ X, Y)" : "Motions (clip @ threshold)");

                // Commit-on-release numeric field (one field edited at a time, keyed by state+child+axis).
                static std::uint32_t s_childEditState = 0;
                static int s_childEditIdx = -1;
                static int s_childEditAxis = 0;
                static float s_childEditVal = 0.0f;
                auto editNum = [&](const char* fid, int childIdx, int axis, float current,
                                   bool& committed) -> float {
                    const bool editing = (s_childEditState == m_animatorSelectedStateId &&
                                          s_childEditIdx == childIdx && s_childEditAxis == axis);
                    float v = editing ? s_childEditVal : current;
                    ImGui::SetNextItemWidth(58.0f);
                    ImGui::DragFloat(fid, &v, 0.05f, 0.0f, 0.0f, "%.2f");
                    if (ImGui::IsItemActivated())
                    {
                        s_childEditState = m_animatorSelectedStateId;
                        s_childEditIdx = childIdx;
                        s_childEditAxis = axis;
                        s_childEditVal = current;
                    }
                    if (ImGui::IsItemActive())
                        s_childEditVal = v;
                    committed = ImGui::IsItemDeactivatedAfterEdit();
                    if (committed || ImGui::IsItemDeactivated())
                    {
                        s_childEditState = 0;
                        s_childEditIdx = -1;
                    }
                    return v;
                };

                int delChild = -1;
                for (int i = 0; i < static_cast<int>(selNode->blendChildren.size()); ++i)
                {
                    ImGui::PushID(i);
                    const AnimatorGraphBlendChild& ch = selNode->blendChildren[i];

                    std::string newClip;
                    if (clipCombo("##c", 130.0f, ch.clipId, newClip))
                    {
                        AnimatorGraphEdit e;
                        e.type = AnimatorGraphEditType::EditBlendTreeChild;
                        e.stateId = m_animatorSelectedStateId;
                        e.intValue = i;
                        e.text = newClip;
                        e.floatValue = ch.threshold;
                        e.vec2Value[0] = ch.pos[0];
                        e.vec2Value[1] = ch.pos[1];
                        m_commands.animatorEdits.push_back(std::move(e));
                    }

                    if (is2D)
                    {
                        ImGui::SameLine();
                        bool cx = false, cy = false;
                        const float nx = editNum("##px", i, 0, ch.pos[0], cx);
                        ImGui::SameLine();
                        const float ny = editNum("##py", i, 1, ch.pos[1], cy);
                        if (cx || cy)
                        {
                            AnimatorGraphEdit e;
                            e.type = AnimatorGraphEditType::EditBlendTreeChild;
                            e.stateId = m_animatorSelectedStateId;
                            e.intValue = i;
                            e.text = ch.clipId;
                            e.floatValue = ch.threshold;
                            e.vec2Value[0] = cx ? nx : ch.pos[0];
                            e.vec2Value[1] = cy ? ny : ch.pos[1];
                            m_commands.animatorEdits.push_back(std::move(e));
                        }
                    }
                    else
                    {
                        ImGui::SameLine();
                        bool commit = false;
                        const float nthr = editNum("##thr", i, 0, ch.threshold, commit);
                        if (commit)
                        {
                            AnimatorGraphEdit e;
                            e.type = AnimatorGraphEditType::EditBlendTreeChild;
                            e.stateId = m_animatorSelectedStateId;
                            e.intValue = i;
                            e.text = ch.clipId;
                            e.floatValue = nthr;
                            e.vec2Value[0] = ch.pos[0];
                            e.vec2Value[1] = ch.pos[1];
                            m_commands.animatorEdits.push_back(std::move(e));
                        }
                    }

                    ImGui::SameLine();
                    if (ImGui::SmallButton("X"))
                        delChild = i;
                    ImGui::PopID();
                }
                if (delChild >= 0)
                {
                    AnimatorGraphEdit e;
                    e.type = AnimatorGraphEditType::DeleteBlendTreeChild;
                    e.stateId = m_animatorSelectedStateId;
                    e.intValue = delChild;
                    m_commands.animatorEdits.push_back(std::move(e));
                }
                if (ImGui::SmallButton("+ Add Motion"))
                {
                    AnimatorGraphEdit e;
                    e.type = AnimatorGraphEditType::AddBlendTreeChild;
                    e.stateId = m_animatorSelectedStateId;
                    e.floatValue = 0.0f;
                    m_commands.animatorEdits.push_back(std::move(e));
                }
            }

            // Speed (commit on release via a backing value so we don't save once per drag-frame).
            static std::uint32_t s_speedEditState = 0;
            static float s_speedEditVal = 1.0f;
            float speedVal = (s_speedEditState == m_animatorSelectedStateId) ? s_speedEditVal : selNode->speed;
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Speed", &speedVal, 0.01f, 0.0f, 10.0f, "%.2f");
            if (ImGui::IsItemActivated())
            {
                s_speedEditState = m_animatorSelectedStateId;
                s_speedEditVal = selNode->speed;
            }
            if (ImGui::IsItemActive())
                s_speedEditVal = speedVal;
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                AnimatorGraphEdit speedEdit;
                speedEdit.type = AnimatorGraphEditType::SetStateSpeed;
                speedEdit.stateId = s_speedEditState;  // the state the drag began on
                speedEdit.floatValue = speedVal;
                m_commands.animatorEdits.push_back(std::move(speedEdit));
                s_speedEditState = 0;
            }
            else if (ImGui::IsItemDeactivated())
            {
                s_speedEditState = 0;
            }

            ImGui::SameLine();
            bool loopVal = selNode->loop;
            if (ImGui::Checkbox("Loop", &loopVal))
            {
                AnimatorGraphEdit loopEdit;
                loopEdit.type = AnimatorGraphEditType::SetStateLoop;
                loopEdit.stateId = m_animatorSelectedStateId;
                loopEdit.boolValue = loopVal;
                m_commands.animatorEdits.push_back(std::move(loopEdit));
            }
            ImGui::EndChild();
        }
    }

    // --- Stage 7: transition editor strip (shown when an edge is selected; mutually exclusive
    //     with the state inspector). Edits a persistent working copy + commits EditTransition. ---
    if (showTransitionEditor)
    {
        // Auto-close if the edge no longer exists (e.g. deleted while selected).
        bool edgeExists = false;
        for (const AnimatorGraphEdge& e : m_animatorGraphState.edges)
            if (e.fromStateId == m_animatorEditEdgeFrom && e.toStateId == m_animatorEditEdgeTo)
            {
                edgeExists = true;
                break;
            }
        if (!edgeExists)
            m_animatorEditEdgeValid = false;
    }
    if (showTransitionEditor && m_animatorEditEdgeValid)
    {
        // Seed the working copy from the snapshot edge whenever the selection changes.
        if (m_animatorEditSeededFrom != m_animatorEditEdgeFrom || m_animatorEditSeededTo != m_animatorEditEdgeTo)
        {
            for (const AnimatorGraphEdge& e : m_animatorGraphState.edges)
                if (e.fromStateId == m_animatorEditEdgeFrom && e.toStateId == m_animatorEditEdgeTo)
                {
                    m_animatorEditHasExitTime = e.hasExitTime;
                    m_animatorEditExitTime = e.exitTime;
                    m_animatorEditDuration = e.duration;
                    m_animatorEditCanSelf = e.canTransitionToSelf;
                    m_animatorEditConditions = e.conditions;
                    break;
                }
            m_animatorEditSeededFrom = m_animatorEditEdgeFrom;
            m_animatorEditSeededTo = m_animatorEditEdgeTo;
        }

        auto nameOf = [&](std::uint32_t id) -> std::string {
            if (id == kAnyStateSentinel) return "Any State";
            for (const AnimatorGraphNode& n : m_animatorGraphState.nodes)
                if (n.id == id) return n.name;
            return "?";
        };
        auto opName = [](AnimEditConditionOp op) -> const char* {
            switch (op)
            {
            case AnimEditConditionOp::Greater: return ">";
            case AnimEditConditionOp::Less: return "<";
            case AnimEditConditionOp::Equals: return "==";
            case AnimEditConditionOp::NotEquals: return "!=";
            case AnimEditConditionOp::If: return "is true";
            case AnimEditConditionOp::IfNot: return "is false";
            }
            return "?";
        };
        auto emitTransition = [&]() {
            AnimatorGraphEdit e;
            e.type = AnimatorGraphEditType::EditTransition;
            e.stateId = m_animatorEditEdgeFrom;
            e.toStateId = m_animatorEditEdgeTo;
            e.hasExitTime = m_animatorEditHasExitTime;
            e.exitTime = m_animatorEditExitTime;
            e.duration = m_animatorEditDuration;
            e.canTransitionToSelf = m_animatorEditCanSelf;
            e.conditions = m_animatorEditConditions;
            m_commands.animatorEdits.push_back(std::move(e));
        };

        ImGui::SetCursorScreenPos(ImVec2(canvasMin.x + 6.0f, canvasMax.y + 4.0f));
        ImGui::BeginChild("##anim_transition_editor", ImVec2(canvasSize.x - 12.0f, inspectorStripH - 8.0f), true);
        ImGui::Text("Transition:  %s  ->  %s",
            nameOf(m_animatorEditEdgeFrom).c_str(), nameOf(m_animatorEditEdgeTo).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Close"))
            m_animatorEditEdgeValid = false;
        ImGui::Separator();

        if (ImGui::Checkbox("Has Exit Time", &m_animatorEditHasExitTime))
            emitTransition();
        if (m_animatorEditHasExitTime)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderFloat("##exit", &m_animatorEditExitTime, 0.0f, 1.0f, "exit %.2f");
            if (ImGui::IsItemDeactivatedAfterEdit())
                emitTransition();
        }
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("Duration (s)", &m_animatorEditDuration, 0.0f, 2.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit())
            emitTransition();
        if (ImGui::Checkbox("Can Transition To Self", &m_animatorEditCanSelf))
            emitTransition();

        ImGui::Separator();
        ImGui::TextDisabled("Conditions (all must pass)");

        int removeIdx = -1;
        for (int i = 0; i < static_cast<int>(m_animatorEditConditions.size()); ++i)
        {
            ImGui::PushID(i);
            AnimatorGraphCondition& c = m_animatorEditConditions[i];
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::BeginCombo("##param", c.param.empty() ? "(param)" : c.param.c_str()))
            {
                for (const AnimatorGraphParameter& p : m_animatorGraphState.parameters)
                    if (ImGui::Selectable(p.name.c_str(), p.name == c.param))
                    {
                        c.param = p.name;
                        emitTransition();
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::BeginCombo("##op", opName(c.op)))
            {
                const AnimEditConditionOp ops[] = {
                    AnimEditConditionOp::Greater, AnimEditConditionOp::Less, AnimEditConditionOp::Equals,
                    AnimEditConditionOp::NotEquals, AnimEditConditionOp::If, AnimEditConditionOp::IfNot};
                for (AnimEditConditionOp o : ops)
                    if (ImGui::Selectable(opName(o), o == c.op))
                    {
                        c.op = o;
                        emitTransition();
                    }
                ImGui::EndCombo();
            }
            if (c.op != AnimEditConditionOp::If && c.op != AnimEditConditionOp::IfNot)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                ImGui::DragFloat("##val", &c.value, 0.05f);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    emitTransition();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X"))
                removeIdx = i;
            ImGui::PopID();
        }
        if (removeIdx >= 0)
        {
            m_animatorEditConditions.erase(m_animatorEditConditions.begin() + removeIdx);
            emitTransition();
        }
        if (ImGui::SmallButton("+ Add Condition"))
        {
            AnimatorGraphCondition c;
            if (!m_animatorGraphState.parameters.empty())
                c.param = m_animatorGraphState.parameters.front().name;
            m_animatorEditConditions.push_back(c);
            emitTransition();
        }
        ImGui::EndChild();
    }

    // --- Stage 7: Parameters panel (left column, controller-global, always shown) ---
    if (m_animatorGraphState.hasController && paramPanelW > 0.0f)
    {
        static int s_paramEditIndex = -1;   // which param's name is being renamed inline
        static int s_paramDefIdx = -1;      // which param's default value is being dragged
        static float s_paramDefVal = 0.0f;

        ImGui::SetCursorScreenPos(origin);
        ImGui::BeginChild("##anim_params", ImVec2(paramPanelW - 4.0f, availSize.y), true);
        ImGui::TextDisabled("Parameters");
        ImGui::Separator();

        const char* typeNames[] = {"Float", "Int", "Bool", "Trigger"};
        int delParamIdx = -1;
        for (int i = 0; i < static_cast<int>(m_animatorGraphState.parameters.size()); ++i)
        {
            ImGui::PushID(i);
            const AnimatorGraphParameter& p = m_animatorGraphState.parameters[i];

            // name row (click to rename inline)
            ImGui::SetNextItemWidth(-1.0f);
            if (s_paramEditIndex == i)
            {
                const bool done = ImGui::InputText("##pname", m_animatorRenameBuf, sizeof(m_animatorRenameBuf),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                if (done || ImGui::IsItemDeactivated())
                {
                    if (m_animatorRenameBuf[0] != '\0' && p.name != m_animatorRenameBuf)
                    {
                        AnimatorGraphEdit e;
                        e.type = AnimatorGraphEditType::RenameParameter;
                        e.text = p.name;
                        e.text2 = m_animatorRenameBuf;
                        m_commands.animatorEdits.push_back(std::move(e));
                    }
                    s_paramEditIndex = -1;
                }
            }
            else if (ImGui::Selectable(p.name.c_str()))
            {
                s_paramEditIndex = i;
                std::size_t k = 0;
                for (; k + 1 < sizeof(m_animatorRenameBuf) && k < p.name.size(); ++k)
                    m_animatorRenameBuf[k] = p.name[k];
                m_animatorRenameBuf[k] = '\0';
            }

            // type combo + default value + delete
            ImGui::SetNextItemWidth(78.0f);
            const int typeIdx = static_cast<int>(p.type);
            if (ImGui::BeginCombo("##ptype", typeNames[typeIdx]))
            {
                for (int t = 0; t < 4; ++t)
                    if (ImGui::Selectable(typeNames[t], t == typeIdx))
                    {
                        AnimatorGraphEdit e;
                        e.type = AnimatorGraphEditType::SetParameterType;
                        e.text = p.name;
                        e.paramType = static_cast<AnimEditParamType>(t);
                        m_commands.animatorEdits.push_back(std::move(e));
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (p.type == AnimEditParamType::Bool)
            {
                bool b = p.defaultValue != 0.0f;
                if (ImGui::Checkbox("##pdef", &b))
                {
                    AnimatorGraphEdit e;
                    e.type = AnimatorGraphEditType::SetParameterDefault;
                    e.text = p.name;
                    e.floatValue = b ? 1.0f : 0.0f;
                    m_commands.animatorEdits.push_back(std::move(e));
                }
            }
            else if (p.type != AnimEditParamType::Trigger)
            {
                float v = (s_paramDefIdx == i) ? s_paramDefVal : p.defaultValue;
                ImGui::SetNextItemWidth(56.0f);
                const bool isInt = (p.type == AnimEditParamType::Int);
                ImGui::DragFloat("##pdef", &v, isInt ? 1.0f : 0.05f, 0.0f, 0.0f, isInt ? "%.0f" : "%.2f");
                if (ImGui::IsItemActivated())
                {
                    s_paramDefIdx = i;
                    s_paramDefVal = p.defaultValue;
                }
                if (ImGui::IsItemActive())
                    s_paramDefVal = v;
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    AnimatorGraphEdit e;
                    e.type = AnimatorGraphEditType::SetParameterDefault;
                    e.text = p.name;
                    e.floatValue = v;
                    m_commands.animatorEdits.push_back(std::move(e));
                    s_paramDefIdx = -1;
                }
                else if (ImGui::IsItemDeactivated())
                {
                    s_paramDefIdx = -1;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X"))
                delParamIdx = i;
            ImGui::Separator();
            ImGui::PopID();
        }
        if (delParamIdx >= 0)
        {
            AnimatorGraphEdit e;
            e.type = AnimatorGraphEditType::DeleteParameter;
            e.text = m_animatorGraphState.parameters[delParamIdx].name;
            m_commands.animatorEdits.push_back(std::move(e));
            s_paramEditIndex = -1;  // indices shift after a delete; cancel any in-progress rename
        }
        if (ImGui::SmallButton("+ Add Parameter"))
        {
            AnimatorGraphEdit e;
            e.type = AnimatorGraphEditType::AddParameter;
            e.text = "NewParam";
            e.paramType = AnimEditParamType::Float;
            m_commands.animatorEdits.push_back(std::move(e));
        }
        ImGui::EndChild();
    }

    ImGui::End();
}
