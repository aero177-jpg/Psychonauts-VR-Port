-- Test-only probe (tools/drive.ps1): camera and player numbers for scale.
function CAGP_hooks(Ob)
	local onPostBeginLevel_original = Ob.onPostBeginLevel
	function Ob:onPostBeginLevel()
		%onPostBeginLevel_original(self)
		local p = Global.player
		GamePrint("psyvr probe: player " .. tostring(p) .. " height " .. tostring(p and p.height))
		if p then
			local px, py, pz = p:getPosition()
			GamePrint("psyvr probe: player pos " .. tostring(px) .. " " .. tostring(py) .. " " .. tostring(pz))
		end
		local cx, cy, cz = GetCameraPosition()
		GamePrint("psyvr probe: camera pos " .. tostring(cx) .. " " .. tostring(cy) .. " " .. tostring(cz))
		local ox, oy, oz = GetCameraOrientation()
		GamePrint("psyvr probe: camera orient " .. tostring(ox) .. " " .. tostring(oy) .. " " .. tostring(oz))
		GamePrint("psyvr probe: fov " .. tostring(GetCameraFieldOfView()))
		GamePrint("psyvr probe: chase radius " .. tostring(GetChaseCameraRadius and GetChaseCameraRadius()) ..
			" altitude " .. tostring(GetChaseCameraAltitude and GetChaseCameraAltitude()) ..
			" target height " .. tostring(GetCameraTargetHeight and GetCameraTargetHeight()))
		GamePrint("psyvr probe: SetChaseCameraRadius " .. tostring(SetChaseCameraRadius) .. " SetCameraShakeEnable " .. tostring(SetCameraShakeEnable))
	end
end
