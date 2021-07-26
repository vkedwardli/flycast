//
//  VulkanView.swift
//  reicast-osx
//
//  Created by Edward Li on 26/7/2021.
//  Copyright © 2021 reicast. All rights reserved.
//

import Cocoa
import MetalKit

class VulkanView: MTKView, NSWindowDelegate {

    override var acceptsFirstResponder: Bool {
        return true;
    }
    
    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        
        render(dirtyRect)
    }
    
    func render(_ dirtyRect: NSRect? = nil) {
        var rect: NSRect!
        if dirtyRect == nil {
            rect = convertToBacking(frame)
        }else{
            rect = convertToBacking(dirtyRect!)
        }
        
        if emu_single_frame(Int32(rect.width), Int32(rect.height)) != 0 {
            emu_vulkan_present()
        }
    }
    
    override func awakeFromNib() {
        //self.wantsBestResolutionOpenGLSurface = true
        let renderTimer = Timer.scheduledTimer(timeInterval: 0.001, target: self, selector: #selector(VulkanView.timerTick), userInfo: nil, repeats: true)
        
        RunLoop.current.add(renderTimer, forMode: .default)
        RunLoop.current.add(renderTimer, forMode: .eventTracking)
        
        let rect = convertToBacking(frame)
        
        guard let metalLayer = self.layer as? CAMetalLayer else {
            return
        }
        
        emu_gles_init(Int32(rect.width), Int32(rect.height), metalLayer)
        
        if (emu_reicast_init() != 0) {
            let alert = NSAlert()
            alert.alertStyle = .critical
            alert.messageText = "Flycast initialization failed"
            alert.runModal()
        }
        
        if let mLayer = self.layer as? CAMetalLayer{
            mLayer.displaySyncEnabled = false
        }
    }
    
   
    @objc func timerTick() {
        if (!emu_renderer_enabled()) {
            NSApplication.shared.terminate(self)
        }
        else if (emu_frame_pending()) {
            self.needsDisplay = true
            //self.render()
        }
    }
    
    override func keyDown(with e: NSEvent) {
        if (!e.isARepeat)
        {
            emu_key_input(e.keyCode, true, UInt32(e.modifierFlags.rawValue & NSEvent.ModifierFlags.deviceIndependentFlagsMask.rawValue))
        }
        emu_character_input(e.characters)
    }
    
    override func keyUp(with e: NSEvent) {
        emu_key_input(e.keyCode, false, UInt32(e.modifierFlags.rawValue & NSEvent.ModifierFlags.deviceIndependentFlagsMask.rawValue))
    }
    
    override func flagsChanged(with e: NSEvent) {
        emu_key_input(0xFF, false, UInt32(e.modifierFlags.rawValue & NSEvent.ModifierFlags.deviceIndependentFlagsMask.rawValue))
    }

    private func setMousePos(_ event: NSEvent)
    {
        let point = convertToBacking(convert(event.locationInWindow, from: self))
        let size = convertToBacking(frame.size)
        emu_set_mouse_position(Int32(point.x), Int32(size.height - point.y), Int32(size.width), Int32(size.height))
    }
    override func mouseDown(with event: NSEvent) {
        emu_mouse_buttons(1, true)
        setMousePos(event)
    }
    override func mouseUp(with event: NSEvent) {
        emu_mouse_buttons(1, false)
        setMousePos(event)
    }
    override func rightMouseDown(with event: NSEvent) {
        emu_mouse_buttons(2, true)
        setMousePos(event)
    }
    override func rightMouseUp(with event: NSEvent) {
        emu_mouse_buttons(2, false)
        setMousePos(event)
    }
    // Not dispatched by default. Need to set Window.acceptsMouseMovedEvents to true
    override func mouseMoved(with event: NSEvent) {
        setMousePos(event)
    }
    override func mouseDragged(with event: NSEvent) {
        emu_mouse_buttons(1, true)
        setMousePos(event)
    }
    override func rightMouseDragged(with event: NSEvent) {
        emu_mouse_buttons(2, true)
        setMousePos(event)
    }
    override func otherMouseDown(with event: NSEvent) {
        emu_mouse_buttons(3, true)
        setMousePos(event)
    }
    override func otherMouseUp(with event: NSEvent) {
        emu_mouse_buttons(3, false)
        setMousePos(event)
    }
    override func scrollWheel(with event: NSEvent) {
        if (event.hasPreciseScrollingDeltas) {
            emu_mouse_wheel(-Float(event.scrollingDeltaY) / 5)
        } else {
            emu_mouse_wheel(-Float(event.scrollingDeltaY) * 10)
        }
    }
    
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        self.window!.delegate = self
        self.window!.acceptsMouseMovedEvents = true
    }
    
    @IBAction func openMenu(_ sender: Any) {
        emu_gui_open_settings();
    }
}
