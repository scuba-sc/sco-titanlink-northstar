untyped
global function TitanLink_Init

// TitanLink's server script: runs only in the local match the TitanLink plugin started
// (TL_OurMatch() is 1 there and 0 in every other game, where this script does nothing at all).
// The plugin's natives (TL_*) exist only when the plugin is loaded: mod.json lists it in
// PluginDependencies and RunOn requires the TitanLink constant, so without the plugin this file
// isn't compiled.

struct
{
	int titanCallSeq = -1
	int embarkSeq = -1
	int disembarkSeq = -1
	int shots = 0
	int lastClip = -1
	entity lastWeapon = null
	bool setup = false
	vector floorPt = < 0, 0, 0 >
	bool haveFloor = false
	bool wasActive = false
} file

void function TitanLink_Init()
{
	AddCallback_OnPlayerRespawned( TitanLink_OnRespawned )
	thread TitanLink_Loop()
}

void function TitanLink_OnRespawned( entity player )
{
	if ( TL_OurMatch() == 0 )
		return
	file.setup = false
	if ( !file.haveFloor )
	{
		file.floorPt = TitanLink_OpenFloor( player )
		file.haveFloor = true
	}
}

// How much open room there is around g: -1 under a roof, else the shortest horizontal ray.
float function TitanLink_Room( entity player, vector g )
{
	TraceResults sky = TraceLine( g + < 0, 0, 100 >, g + < 0, 0, 2400 >, player, TRACE_MASK_SOLID, TRACE_COLLISION_GROUP_NONE )
	if ( sky.fraction < 0.5 )
		return -1.0
	float worst = 99999.0
	for ( int i = 0; i < 16; i++ )
	{
		vector dir = AnglesToForward( < 0, float( i ) * 22.5, 0 > )
		for ( int k = 0; k < 2; k++ )
		{
			float h = 90.0
			if ( k == 1 )
				h = 380.0
			vector a = < g.x, g.y, g.z + h >
			TraceResults r = TraceLine( a, a + dir * 2000.0, player, TRACE_MASK_SOLID, TRACE_COLLISION_GROUP_NONE )
			float d = r.fraction * 2000.0
			if ( d < worst )
				worst = d
		}
	}
	return worst
}

// The anchor: the spawn spot with the most open room, so a Titan fits where Star Citizen stands.
vector function TitanLink_OpenFloor( entity player )
{
	array<vector> spots = [ player.GetOrigin() ]
	foreach ( entity e in SpawnPoints_GetTitan() )
		spots.append( e.GetOrigin() )
	foreach ( entity e in SpawnPoints_GetTitanStart( player.GetTeam() ) )
		spots.append( e.GetOrigin() )
	foreach ( entity e in SpawnPoints_GetPilot() )
		spots.append( e.GetOrigin() )
	vector best = player.GetOrigin()
	float bestRoom = -2.0
	int n = 0
	foreach ( vector s in spots )
	{
		if ( n >= 64 )
			break
		n++
		TraceResults down = TraceLine( s + < 0, 0, 64 >, s - < 0, 0, 512 >, player, TRACE_MASK_SOLID, TRACE_COLLISION_GROUP_NONE )
		vector g = down.fraction < 1.0 ? down.endPos : s
		float room = TitanLink_Room( player, g )
		if ( room > bestRoom )
		{
			bestRoom = room
			best = g
		}
	}
	TL_Log( "anchor at " + best + " with " + bestRoom + " units of room, out of " + n + " spots" )
	return best
}

void function TitanLink_Loop()
{
	float holdAt = 0.0
	for ( ;; )
	{
		WaitFrame()
		if ( TL_OurMatch() == 0 )
			continue
		// The local match doesn't end on its own while you play it from Star Citizen.
		if ( Time() > holdAt && GetGameState() == eGameState.Playing )
		{
			holdAt = Time() + 5.0
			SetServerVar( "gameEndTime", Time() + 3600.0 )
			SetServerVar( "roundEndTime", Time() + 3600.0 )
		}
		array<entity> players = GetPlayerArray()
		if ( players.len() == 0 )
			continue
		TitanLink_Tick( players[0], players.len() )
	}
}

void function TitanLink_DropTitan( entity player )
{
	if ( player.IsTitan() )
		return
	if ( player.s.replacementDropInProgress )
	{
		TL_Log( "a Titan is already dropping" )
		return
	}
	entity pet = player.GetPetTitan()
	if ( IsAlive( pet ) )
		pet.Destroy()
	Point p
	p.origin = TL_TitanDrop()
	p.angles = < 0, TL_TitanDropYaw(), 0 >
	TL_Log( "titanfall at " + p.origin )
	thread CreateTitanForPlayerAndHotdrop( player, p )
}

void function TitanLink_Embark( entity player )
{
	entity pet = player.GetPetTitan()
	if ( !IsAlive( pet ) || player.IsTitan() )
		return
	if ( player.IsNoclipping() )
		player.SetPhysics( MOVETYPE_WALK )
	local embark = FindBestEmbark( player, pet, false )
	if ( embark == null )
	{
		TL_Log( "no embark animation fits here" )
		return
	}
	thread PlayerEmbarksTitan( player, pet, expect table( embark ) )
}

void function TitanLink_Disembark( entity player )
{
	if ( !player.IsTitan() )
		return
	if ( player.IsNoclipping() )
		player.SetPhysics( MOVETYPE_WALK )
	thread PlayerDisembarksTitan( player )
}

bool function TitanLink_Busy( entity player )
{
	if ( player.ContextAction_IsActive() || IsValid( player.GetParent() ) )
		return true
	return IsPlayerEmbarking( player ) || IsPlayerDisembarking( player )
}

// A counter from the plugin moved since we last looked (the first look only records it).
bool function TitanLink_Moved( int now, int last )
{
	return last >= 0 && now != last
}

void function TitanLink_Tick( entity player, int playerCount )
{
	int sc = TL_Flags()
	bool active = ( sc & 1 ) != 0 && ( sc & 4 ) != 0 // pilot mode, and the pose is valid
	int report = 0
	vector origin = < 0, 0, 0 >
	float eyeZ = 0.0
	vector titanOrigin = < 0, 0, 0 >
	float titanYaw = 0.0
	int clip = -1
	float damage = 0.0
	float titanHealth = 0.0
	string weaponName = ""
	float zoomFrac = 0.0
	float zoomFov = 0.0

	if ( IsValid( player ) && IsAlive( player ) )
	{
		report = report | 1
		if ( !file.haveFloor )
		{
			file.floorPt = TitanLink_OpenFloor( player )
			file.haveFloor = true
		}
		bool busy = TitanLink_Busy( player )
		if ( busy )
			report = report | 8
		if ( player.IsTitan() )
		{
			report = report | 2
			titanHealth = GetHealthFrac( player )
		}
		entity pet = player.GetPetTitan()
		if ( IsAlive( pet ) )
		{
			report = report | 4
			titanOrigin = pet.GetOrigin()
			titanYaw = pet.GetAngles().y
			titanHealth = GetHealthFrac( pet )
			if ( player.GetPetTitanMode() != eNPCTitanMode.STAY )
			{
				player.SetPetTitanMode( eNPCTitanMode.STAY )
				pet.DisableBehavior( "Follow" )
			}
		}

		int callSeq = TL_TitanCallSeq()
		int embarkSeq = TL_EmbarkSeq()
		int disembarkSeq = TL_DisembarkSeq()
		if ( active )
		{
			if ( !file.setup )
			{
				file.setup = true
				player.SetInvulnerable()
				player.SetNoTarget( true )
				player.SetNoTargetSmartAmmo( true )
				TL_Log( "the pilot now follows Star Citizen" )
			}
			if ( TitanLink_Moved( callSeq, file.titanCallSeq ) )
				TitanLink_DropTitan( player )
			if ( TitanLink_Moved( embarkSeq, file.embarkSeq ) && !busy )
				TitanLink_Embark( player )
			if ( TitanLink_Moved( disembarkSeq, file.disembarkSeq ) && !busy )
			{
				player.SetOrigin( TL_Feet() )
				TitanLink_Disembark( player )
			}
			busy = TitanLink_Busy( player )
			if ( !busy )
			{
				if ( !player.IsNoclipping() )
					player.SetPhysics( MOVETYPE_NOCLIP )
				player.SetOrigin( TL_Feet() )
				player.SetVelocity( < 0, 0, 0 > )
			}
		}
		else if ( file.wasActive )
		{
			if ( player.IsNoclipping() && !IsValid( player.GetParent() ) )
				player.SetPhysics( MOVETYPE_WALK )
			player.ClearInvulnerable()
			player.SetNoTarget( false )
			player.SetNoTargetSmartAmmo( false )
			file.setup = false
			TL_Log( "the pilot is released" )
		}
		file.titanCallSeq = callSeq
		file.embarkSeq = embarkSeq
		file.disembarkSeq = disembarkSeq
		file.wasActive = active

		entity weapon = player.GetActiveWeapon()
		if ( IsValid( weapon ) )
		{
			weaponName = weapon.GetWeaponClassName()
			clip = weapon.GetWeaponPrimaryClipCount()
			if ( weapon == file.lastWeapon && file.lastClip >= 0 && clip < file.lastClip )
				file.shots += file.lastClip - clip
			file.lastWeapon = weapon
			file.lastClip = clip
			local d = weapon.GetWeaponInfoFileKeyField( "damage_near_value" )
			if ( d != null )
				damage = expect float( d.tofloat() )
			local z = weapon.GetWeaponInfoFileKeyField( "zoom_fov" )
			if ( z != null )
				zoomFov = expect float( z.tofloat() )
		}
		origin = player.GetOrigin()
		eyeZ = player.EyePosition().z - origin.z
		zoomFrac = player.GetZoomFrac()
	}
	TL_Report( report, origin, eyeZ, titanOrigin, titanYaw, clip, file.shots, damage, titanHealth, file.floorPt, weaponName, zoomFrac, zoomFov, playerCount )
}
