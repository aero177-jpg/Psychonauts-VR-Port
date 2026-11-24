-- Test-only (tools/drive.ps1 Quick-Vr -Continue): when the main menu level
-- starts, pick "Continue" (MainMenuProcessSelection(2), found 2026-10-21),
-- which loads the most recent save. Installed for the test run, removed after.
function STMU_hooks(Ob)
	local onPostBeginLevel_original = Ob.onPostBeginLevel
	function Ob:onPostBeginLevel()
		%onPostBeginLevel_original(self)
		GamePrint("psyvr: test hook - Continue")
		MainMenuProcessSelection(2)
	end
end
