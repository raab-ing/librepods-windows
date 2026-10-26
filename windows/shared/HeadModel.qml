import QtQuick

// Stylized head tracker built like an instrument dial: a fixed compass
// ring with degree ticks and a yaw marker that rides it, and a flat head
// glyph that foreshortens inside the ring as it turns. The ring never
// rotates — it is the reference frame the head turns within. A Timer
// chase drives smoothing so the pose is live even after the window was
// hidden (Qt pauses Behavior animations on hidden windows, which used to
// strand the pose on revisit).
Item {
    id: root
    property real yaw: 0      // -90..90, + = turned right
    property real pitch: 0    // -90..90, + = tilted down
    // Visual amplification of the calibrated angles — the glyph reads
    // better slightly over-rotated; the numeric readout stays honest.
    property real gain: 1.0
    property real dispYaw: 0
    property real dispPitch: 0
    property color lineColor: "#30D158"   // accent — stroke, nose, pods, marker
    property color dimColor: "#636366"
    // Head/ear fills — defaults match the dark theme; Main.qml feeds light
    // variants so the glyph isn't a dark blob on a light card.
    property color fillTop: "#4a4a54"
    property color fillBottom: "#2c2c33"
    property color earColor: "#4a4a54"
    // Brief accent flare on the dial when a head gesture fires — wired from
    // Main.qml's onLastGestureChanged. 0 = idle, 1 = full flash.
    property real flash: 0
    function gestureFlash() {
        if (typeof airPodsTrayApp !== "undefined" && airPodsTrayApp.reduceMotion) return
        flashAnim.restart()
    }
    SequentialAnimation {
        id: flashAnim
        NumberAnimation { target: root; property: "flash"; to: 1; duration: 140; easing.type: Easing.OutCubic }
        NumberAnimation { target: root; property: "flash"; to: 0; duration: 560; easing.type: Easing.OutCubic }
    }

    implicitWidth: 130
    implicitHeight: 130

    Timer {
        interval: 16
        repeat: true
        running: root.visible
        onTriggered: {
            const ty = root.yaw * root.gain
            const tp = root.pitch * root.gain
            root.dispYaw += (ty - root.dispYaw) * 0.4
            root.dispPitch += (tp - root.dispPitch) * 0.4
            if (Math.abs(ty - root.dispYaw) < 0.05) root.dispYaw = ty
            if (Math.abs(tp - root.dispPitch) < 0.05) root.dispPitch = tp
        }
    }

    // ---- static compass dial: ring, degree ticks, yaw marker ----
    Canvas {
        id: dial
        anchors.fill: parent
        z: 0

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const W = width, H = height, cx = W / 2, cy = H / 2
            const ringR = Math.min(W, H) / 2 - 4
            const accent = root.lineColor

            ctx.strokeStyle = root.dimColor
            ctx.lineWidth = 1.2
            ctx.globalAlpha = 0.55
            ctx.beginPath()
            // NB: Qt Context2D.ellipse() is (x, y, w, h) — top-left +
            // dimensions, unlike HTML5's (cx, cy, rx, ry, ...). Passing
            // center+radius draws the circle shifted down-right.
            ctx.ellipse(cx - ringR, cy - ringR, ringR * 2, ringR * 2)
            ctx.stroke()
            for (let i = 0; i < 12; ++i) {
                const a = i * Math.PI / 6
                const cardinal = i % 3 === 0
                const inner = ringR - (cardinal ? 7 : 4)
                ctx.globalAlpha = cardinal ? 0.85 : 0.45
                ctx.strokeStyle = cardinal ? accent : root.dimColor
                ctx.beginPath()
                ctx.moveTo(cx + Math.cos(a) * inner, cy + Math.sin(a) * inner)
                ctx.lineTo(cx + Math.cos(a) * ringR, cy + Math.sin(a) * ringR)
                ctx.stroke()
            }
            ctx.globalAlpha = 1

            // Yaw marker — an accent dot riding the ring at the live
            // heading: 12 o'clock facing you, orbiting toward 3/9 as the
            // head turns. This is what makes the dial a tracker.
            const ha = (-90 + Math.max(-80, Math.min(80, root.dispYaw))) * Math.PI / 180
            const mx = cx + Math.cos(ha) * ringR
            const my = cy + Math.sin(ha) * ringR
            ctx.fillStyle = accent
            ctx.globalAlpha = 0.25
            ctx.beginPath()
            ctx.ellipse(mx - 5.2, my - 5.2, 10.4, 10.4)
            ctx.fill()
            ctx.globalAlpha = 0.95
            ctx.beginPath()
            ctx.ellipse(mx - 2.6, my - 2.6, 5.2, 5.2)
            ctx.fill()
            ctx.globalAlpha = 1

            // Gesture flare — a brighter accent ring over the dial.
            if (root.flash > 0.01) {
                ctx.strokeStyle = accent
                ctx.globalAlpha = root.flash * 0.85
                ctx.lineWidth = 2.4
                ctx.beginPath()
                ctx.ellipse(cx - ringR, cy - ringR, ringR * 2, ringR * 2)
                ctx.stroke()
                ctx.globalAlpha = 1
                ctx.lineWidth = 1.2
            }
        }
    }

    // ---- rotating stage: head glyph, ears, pods, nose ----
    Item {
        id: headStage
        anchors.fill: parent
        z: 1

        transform: [
            Rotation {
                origin.x: headStage.width / 2; origin.y: headStage.height / 2
                axis { x: 0; y: 1; z: 0 }
                // Soft-saturate at ±48° — beyond that a squashed face-on
                // silhouette reads as a thin egg, not a turned head; the
                // dial marker carries the true extreme heading instead.
                angle: Math.max(-48, Math.min(48, root.dispYaw)) * 0.85
            },
            Rotation {
                origin.x: headStage.width / 2; origin.y: headStage.height / 2
                axis { x: 1; y: 0; z: 0 }
                angle: Math.max(-55, Math.min(55, -root.dispPitch)) * 0.8
            }
        ]

        Canvas {
            id: face
            anchors.fill: parent

            function headPath(ctx, cx, cy) {
                // Face-on head silhouette: rounded crown, tapering
                // cheeks, soft jaw and chin.
                ctx.beginPath()
                ctx.moveTo(cx, cy - 32)
                ctx.bezierCurveTo(cx + 13, cy - 32, cx + 20, cy - 22, cx + 20.5, cy - 8)   // crown → temple
                ctx.bezierCurveTo(cx + 21, cy + 2, cx + 19.5, cy + 9, cx + 14, cy + 18)    // cheek → jaw
                ctx.bezierCurveTo(cx + 11, cy + 24, cx + 6.5, cy + 28.5, cx, cy + 28.5)    // jaw → chin
                ctx.bezierCurveTo(cx - 6.5, cy + 28.5, cx - 11, cy + 24, cx - 14, cy + 18) // chin → jaw
                ctx.bezierCurveTo(cx - 19.5, cy + 9, cx - 21, cy + 2, cx - 20.5, cy - 8)   // jaw → cheek
                ctx.bezierCurveTo(cx - 20, cy - 22, cx - 13, cy - 32, cx, cy - 32)         // temple → crown
                ctx.closePath()
            }

            function podAt(ctx, x, y, s, flip, accent, stemCol) {
                // Tiny AirPod: light capsule head + darker angled stem.
                ctx.save()
                ctx.translate(x, y)
                ctx.scale(flip ? -s : s, s)
                ctx.strokeStyle = stemCol
                ctx.lineWidth = 1.7
                ctx.lineCap = "round"
                ctx.beginPath()
                ctx.moveTo(0.9, 1.0)
                ctx.quadraticCurveTo(1.7, 4.6, 1.0, 7.6)
                ctx.stroke()
                ctx.fillStyle = accent
                ctx.beginPath()
                ctx.ellipse(-2.7, -1.6 - 3.7, 5.4, 7.4)
                ctx.fill()
                ctx.fillStyle = "rgba(0,0,0,0.35)"
                ctx.beginPath()
                ctx.ellipse(-0.6 - 0.7, -2.6 - 0.9, 1.4, 1.8)
                ctx.fill()
                ctx.restore()
            }

            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const W = width, H = height, cx = W / 2, cy = H / 2
                const yawR = Math.max(-48, Math.min(48, root.dispYaw)) * Math.PI / 180
                const pitchR = Math.max(-55, Math.min(55, root.dispPitch)) * Math.PI / 180
                const pN = Math.max(-55, Math.min(55, root.dispPitch)) / 55   // -1..1
                const fyaw = Math.sin(yawR)
                const spread = Math.cos(yawR)          // 1 facing, 0 profile
                const vComp = Math.cos(pitchR)
                const fx = fyaw * 13                   // feature parallax
                const fy = Math.sin(pitchR) * 12       // vertical parallax — must
                // rival fx or nods read as "nothing happened" (was *8 ≈ 2px
                // of motion at a normal nod — invisible)
                const accent = root.lineColor
                // Silhouette slides on nod and foreshortens — the face-on
                // view of a head rotating toward/away from you. Linear in
                // pN so even small tilts read: a 10° tilt drops the face
                // ~5px, a deep nod 16px + 30% squash.
                const headDrop = pN * 16
                const squash = 1 - Math.abs(pN) * 0.30

                ctx.save()
                ctx.translate(0, cy * (1 - squash) + headDrop)
                ctx.scale(1, squash)

                // Faint accent halo under the stroke.
                headPath(ctx, cx, cy)
                ctx.save()
                ctx.shadowColor = accent
                ctx.shadowBlur = 10
                ctx.globalAlpha = 0.20
                ctx.fillStyle = accent
                ctx.fill()
                ctx.restore()

                // Flat fill — a quiet vertical gradient, darker at the chin.
                headPath(ctx, cx, cy)
                const fill = ctx.createLinearGradient(cx, cy - 32, cx, cy + 29)
                fill.addColorStop(0, root.fillTop)
                fill.addColorStop(1, root.fillBottom)
                ctx.fillStyle = fill
                ctx.fill()

                // Moving key light — this is what the mannequin had and the
                // flat glyph lost: the eye reads tilt from *light sliding
                // across the face*, not from silhouette movement. The
                // highlight tracks pitch/yaw like a lamp fixed up-left of
                // the viewer, so a nod drags the bright spot down the face
                // even when the silhouette barely shifts.
                ctx.save()
                headPath(ctx, cx, cy)
                ctx.clip()
                const keyX = cx - 10 - fx * 0.35, keyY = cy - 16 - fy * 0.55
                const key = ctx.createRadialGradient(keyX, keyY, 3, keyX, keyY, 46)
                key.addColorStop(0, "rgba(255,255,255,0.22)")
                key.addColorStop(0.5, "rgba(255,255,255,0.06)")
                key.addColorStop(1, "rgba(255,255,255,0)")
                ctx.fillStyle = key
                ctx.fillRect(0, 0, W, H)
                ctx.restore()

                // Chin/neck shadow — grows as the head tips forward, exactly
                // the cue real heads give: the throat disappears under the jaw.
                ctx.save()
                headPath(ctx, cx, cy)
                ctx.clip()
                const shA = 0.10 + Math.max(0, pN) * 0.38
                const shade = ctx.createRadialGradient(cx, cy + 30 + headDrop * 0.4, 1,
                                                       cx, cy + 30 + headDrop * 0.4, 14)
                shade.addColorStop(0, "rgba(0,0,0," + shA + ")")
                shade.addColorStop(1, "rgba(0,0,0,0)")
                ctx.fillStyle = shade
                ctx.beginPath()
                ctx.ellipse(cx - 11, cy + 24, 22, 11)
                ctx.fill()
                ctx.restore()

                // Crisp accent outline.
                headPath(ctx, cx, cy)
                ctx.strokeStyle = accent
                ctx.globalAlpha = 0.85
                ctx.lineWidth = 1.4
                ctx.stroke()
                ctx.globalAlpha = 1
                ctx.restore()

                // Crown reveal — nodding exposes the top of the head; an
                // accent scalp wedge that fades in past a small threshold.
                if (pN > 0.08) {
                    ctx.fillStyle = accent
                    ctx.globalAlpha = Math.min(0.5, pN * 0.7)
                    ctx.beginPath()
                    ctx.ellipse(cx - 13 * pN - 2, cy - 30 + headDrop - 4 * pN,
                                26 * pN + 4, 9 * pN + 2)
                    ctx.fill()
                    ctx.globalAlpha = 1
                }
                // Chin reveal — tilting back exposes the jaw's underside.
                if (pN < -0.08) {
                    ctx.fillStyle = accent
                    ctx.globalAlpha = Math.min(0.4, -pN * 0.6)
                    ctx.beginPath()
                    ctx.ellipse(cx - 10 * -pN - 1, cy + 24 + headDrop,
                                20 * -pN + 2, 7 * -pN + 1.5)
                    ctx.fill()
                    ctx.globalAlpha = 1
                }

                // ---- ears, sized by near/far side; fade out toward
                // profile so the extreme pose is a clean silhouette ----
                const earY = cy + headDrop * 0.9 + fy * 0.25
                const earLW = Math.max(0.8, 3.4 * (0.45 + spread * 0.55 - fyaw * 0.55))
                const earRW = Math.max(0.8, 3.4 * (0.45 + spread * 0.55 + fyaw * 0.55))
                ctx.globalAlpha = 0.35 + spread * 0.65
                const earH = 5.2 * vComp + 0.5
                ctx.fillStyle = root.earColor
                ctx.beginPath()
                ctx.ellipse(cx - 20 + fx * 0.1 - earLW, earY - earH, earLW * 2, earH * 2)
                ctx.fill()
                ctx.beginPath()
                ctx.ellipse(cx + 20 + fx * 0.1 - earRW, earY - earH, earRW * 2, earH * 2)
                ctx.fill()
                ctx.globalAlpha = 1

                // ---- AirPods in the ears ----
                if (earLW > 1.6)
                    podAt(ctx, cx - 20.5 + fx * 0.12, earY + 0.5, Math.min(1, earLW / 3.4), true, "#E9E9EE", accent)
                if (earRW > 1.6)
                    podAt(ctx, cx + 20.5 + fx * 0.12, earY + 0.5, Math.min(1, earRW / 3.4), false, "#E9E9EE", accent)

                // ---- direction marker: accent "nose" triangle. On a nod it
                // climbs toward the crown and shortens — the same parallax
                // the face features get when the head tips forward. Past
                // profile it pushes to the silhouette's edge and widens —
                // an edge protrusion reads as a profile nose, which is what
                // keeps the extreme pose looking like a head.
                const profile = Math.max(0, Math.min(1, (Math.abs(root.dispYaw) - 50) / 20))
                const noseX = cx + fx * (1.05 + profile * 0.9)
                const noseTop = cy - 1 + fy * 0.7 - Math.max(0, pN) * 5
                const noseBot = cy + 8 * Math.max(0.4, vComp) + fy * 0.5
                const noseW = Math.max(0.9, 2.6 * spread) * (1 + profile * 0.7)
                ctx.fillStyle = accent
                ctx.globalAlpha = 0.9
                ctx.beginPath()
                ctx.moveTo(noseX, noseTop)
                ctx.lineTo(noseX - noseW, noseBot)
                ctx.lineTo(noseX + noseW, noseBot)
                ctx.closePath()
                ctx.fill()
                ctx.globalAlpha = 1
            }
        }
    }

    onDispYawChanged: { face.requestPaint(); dial.requestPaint() }
    onDispPitchChanged: face.requestPaint()
    onFlashChanged: dial.requestPaint()
    onLineColorChanged: { face.requestPaint(); dial.requestPaint() }
    onVisibleChanged: if (visible) { face.requestPaint(); dial.requestPaint() }
    Component.onCompleted: { face.requestPaint(); dial.requestPaint() }
}
