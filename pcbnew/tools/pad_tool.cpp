/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2017-2019 KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */


#include "pad_tool.h"
#include <class_draw_panel_gal.h>
#include <view/view_controls.h>
#include <view/view.h>
#include <tool/tool_manager.h>
#include <bitmaps.h>
#include <class_board_item.h>
#include <class_module.h>
#include <board_commit.h>
#include <dialogs/dialog_push_pad_properties.h>
#include <tools/pcb_actions.h>
#include <tools/selection_tool.h>
#include <tools/pcb_selection_conditions.h>
#include <tools/edit_tool.h>
#include <dialogs/dialog_enum_pads.h>


PAD_TOOL::PAD_TOOL() :
        PCB_TOOL_BASE( "pcbnew.PadTool" ),
        m_padCopied( false )
{}


PAD_TOOL::~PAD_TOOL()
{}


void PAD_TOOL::Reset( RESET_REASON aReason )
{
    m_padCopied = false;
}


bool PAD_TOOL::Init()
{
    SELECTION_TOOL* selTool = m_toolMgr->GetTool<SELECTION_TOOL>();

    if( selTool )
    {
        // Add context menu entries that are displayed when selection tool is active
        CONDITIONAL_MENU& menu = selTool->GetToolMenu().GetMenu();

        SELECTION_CONDITION padSel = SELECTION_CONDITIONS::HasType( PCB_PAD_T );
        SELECTION_CONDITION singlePadSel = SELECTION_CONDITIONS::Count( 1 ) &&
                                           SELECTION_CONDITIONS::OnlyType( PCB_PAD_T );

        menu.AddSeparator( 400 );

        if( m_editModules )
        {
            menu.AddItem( PCB_ACTIONS::createPadFromShapes,  SELECTION_CONDITIONS::NotEmpty, 400 );
            menu.AddItem( PCB_ACTIONS::explodePadToShapes,   singlePadSel, 400 );
        }

        menu.AddItem( PCB_ACTIONS::copyPadSettings,      singlePadSel, 400 );
        menu.AddItem( PCB_ACTIONS::applyPadSettings,     padSel, 400 );
        menu.AddItem( PCB_ACTIONS::pushPadSettings,      singlePadSel, 400 );
        menu.AddItem( PCB_ACTIONS::enumeratePads,        SELECTION_CONDITIONS::ShowAlways, 400 );
    }

    return true;
}


int PAD_TOOL::pastePadProperties( const TOOL_EVENT& aEvent )
{
    auto& selTool = *m_toolMgr->GetTool<SELECTION_TOOL>();
    const auto& selection = selTool.GetSelection();
    const D_PAD& masterPad = frame()->GetDesignSettings().m_Pad_Master;

    BOARD_COMMIT commit( frame() );

    // for every selected pad, paste global settings
    for( auto item : selection )
    {
        if( item->Type() == PCB_PAD_T )
        {
            commit.Modify( item );
            static_cast<D_PAD&>( *item ).ImportSettingsFrom( masterPad );
        }
    }

    commit.Push( _( "Paste Pad Properties" ) );

    m_toolMgr->ProcessEvent( EVENTS::SelectedItemsModified );
    frame()->Refresh();

    return 0;
}


int PAD_TOOL::copyPadSettings( const TOOL_EVENT& aEvent )
{
    auto& selTool = *m_toolMgr->GetTool<SELECTION_TOOL>();
    const auto& selection = selTool.GetSelection();

    D_PAD& masterPad = frame()->GetDesignSettings().m_Pad_Master;

    // can only copy from a single pad
    if( selection.Size() == 1 )
    {
        auto item = selection[0];

        if( item->Type() == PCB_PAD_T )
        {
            const auto& selPad = static_cast<const D_PAD&>( *item );
            masterPad.ImportSettingsFrom( selPad );
            m_padCopied = true;
        }
    }

    return 0;
}


static void doPushPadProperties( BOARD& board, const D_PAD& aSrcPad, BOARD_COMMIT& commit,
                                 bool aSameFootprints,
                                 bool aPadShapeFilter,
                                 bool aPadOrientFilter,
                                 bool aPadLayerFilter,
                                 bool aPadTypeFilter )
{
    const MODULE* moduleRef = aSrcPad.GetParent();

    double pad_orient = aSrcPad.GetOrientation() - moduleRef->GetOrientation();

    for( auto module : board.Modules() )
    {
        if( !aSameFootprints && ( module != moduleRef ) )
            continue;

        if( module->GetFPID() != moduleRef->GetFPID() )
            continue;

        for( auto pad : module->Pads() )
        {
            if( aPadShapeFilter && ( pad->GetShape() != aSrcPad.GetShape() ) )
                continue;

            double currpad_orient = pad->GetOrientation() - module->GetOrientation();

            if( aPadOrientFilter && ( currpad_orient != pad_orient ) )
                continue;

            if( aPadLayerFilter && ( pad->GetLayerSet() != aSrcPad.GetLayerSet() ) )
                continue;

            if( aPadTypeFilter && ( pad->GetAttribute() != aSrcPad.GetAttribute() ) )
                    continue;

            // Special-case for aperture pads
            if( aPadTypeFilter && pad->GetAttribute() == PAD_ATTRIB_CONN )
            {
                if( pad->IsAperturePad() != aSrcPad.IsAperturePad() )
                    continue;
            }

            commit.Modify( pad );

            // Apply source pad settings to this pad
            pad->ImportSettingsFrom( aSrcPad );
        }
    }
}


int PAD_TOOL::pushPadSettings( const TOOL_EVENT& aEvent )
{
    auto&       selTool = *m_toolMgr->GetTool<SELECTION_TOOL>();
    const auto& selection = selTool.GetSelection();
    D_PAD*      srcPad;

    if( selection.Size() == 1 && selection[0]->Type() == PCB_PAD_T )
        srcPad = static_cast<D_PAD*>( selection[0] );
    else
        return 0;

    MODULE* module = srcPad->GetParent();

    if( !module )
        return 0;

    frame()->SetMsgPanel( module );

    DIALOG_PUSH_PAD_PROPERTIES dlg( frame() );
    int dialogRet = dlg.ShowModal();

    if( dialogRet == wxID_CANCEL )
        return 0;

    const bool edit_Same_Modules = (dialogRet == 1);

    BOARD_COMMIT commit( frame() );

    doPushPadProperties( *getModel<BOARD>(), *srcPad, commit, edit_Same_Modules,
                         DIALOG_PUSH_PAD_PROPERTIES::m_Pad_Shape_Filter,
                         DIALOG_PUSH_PAD_PROPERTIES::m_Pad_Orient_Filter,
                         DIALOG_PUSH_PAD_PROPERTIES::m_Pad_Layer_Filter,
                         DIALOG_PUSH_PAD_PROPERTIES::m_Pad_Type_Filter );

    commit.Push( _( "Push Pad Settings" ) );

    m_toolMgr->ProcessEvent( EVENTS::SelectedItemsModified );
    frame()->Refresh();

    return 0;
}


int PAD_TOOL::EnumeratePads( const TOOL_EVENT& aEvent )
{
    if( !board()->GetFirstModule() || board()->GetFirstModule()->Pads().empty() )
        return 0;

    DIALOG_ENUM_PADS settingsDlg( frame() );

    if( settingsDlg.ShowModal() != wxID_OK )
        return 0;

    std::string tool = aEvent.GetCommandStr().get();
    frame()->PushTool( tool );
    Activate();

    GENERAL_COLLECTOR collector;
    const KICAD_T types[] = { PCB_PAD_T, EOT };

    GENERAL_COLLECTORS_GUIDE guide = frame()->GetCollectorsGuide();
    guide.SetIgnoreMTextsMarkedNoShow( true );
    guide.SetIgnoreMTextsOnBack( true );
    guide.SetIgnoreMTextsOnFront( true );
    guide.SetIgnoreModulesVals( true );
    guide.SetIgnoreModulesRefs( true );

    int seqPadNum = settingsDlg.GetStartNumber();
    wxString padPrefix = settingsDlg.GetPrefix();
    std::deque<int> storedPadNumbers;

    m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );
    getViewControls()->ShowCursor( true );

    KIGFX::VIEW* view = m_toolMgr->GetView();
    VECTOR2I oldCursorPos;  // store the previous mouse cursor position, during mouse drag
    std::list<D_PAD*> selectedPads;
    BOARD_COMMIT commit( frame() );
    std::map<wxString, std::pair<int, wxString>> oldNames;
    bool isFirstPoint = true;   // used to be sure oldCursorPos will be initialized at least once.

    STATUS_TEXT_POPUP statusPopup( frame() );
    wxString msg = _( "Click on pad %s%d\nPress <esc> to cancel or double-click to commit" );
    statusPopup.SetText( wxString::Format( msg, padPrefix, seqPadNum ) );
    statusPopup.Popup();
    statusPopup.Move( wxGetMousePosition() + wxPoint( 20, 20 ) );

    while( TOOL_EVENT* evt = Wait() )
    {
        frame()->GetCanvas()->SetCurrentCursor( wxCURSOR_BULLSEYE );

        if( evt->IsCancelInteractive() )
        {
            m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );
            commit.Revert();

            frame()->PopTool( tool );
            break;
        }

        else if( evt->IsActivate() )
        {
            commit.Push( _( "Renumber pads" ) );

            frame()->PopTool( tool );
            break;
        }

        else if( evt->IsDrag( BUT_LEFT ) || evt->IsClick( BUT_LEFT ) )
        {
            selectedPads.clear();
            VECTOR2I cursorPos = getViewControls()->GetCursorPosition();

            // Be sure the old cursor mouse position was initialized:
            if( isFirstPoint )
            {
                oldCursorPos = cursorPos;
                isFirstPoint = false;
            }

            // wxWidgets deliver mouse move events not frequently enough, resulting in skipping
            // pads if the user moves cursor too fast. To solve it, create a line that approximates
            // the mouse move and search pads that are on the line.
            int distance = ( cursorPos - oldCursorPos ).EuclideanNorm();
            // Search will be made every 0.1 mm:
            int segments = distance / int( 0.1*IU_PER_MM ) + 1;
            const wxPoint line_step( ( cursorPos - oldCursorPos ) / segments );

            collector.Empty();

            for( int j = 0; j < segments; ++j )
            {
                wxPoint testpoint( cursorPos.x - j * line_step.x, cursorPos.y - j * line_step.y );
                collector.Collect( board(), types, testpoint, guide );

                for( int i = 0; i < collector.GetCount(); ++i )
                    selectedPads.push_back( static_cast<D_PAD*>( collector[i] ) );
            }

            selectedPads.unique();

            for( D_PAD* pad : selectedPads )
            {
                // If pad was not selected, then enumerate it
                if( !pad->IsSelected() )
                {
                    commit.Modify( pad );

                    // Rename pad and store the old name
                    int newval;

                    if( storedPadNumbers.size() > 0 )
                    {
                        newval = storedPadNumbers.front();
                        storedPadNumbers.pop_front();
                    }
                    else
                        newval = seqPadNum++;

                    wxString newName = wxString::Format( wxT( "%s%d" ), padPrefix, newval );
                    oldNames[newName] = { newval, pad->GetName() };
                    pad->SetName( newName );
                    pad->SetSelected();
                    getView()->Update( pad );

                    // Ensure the popup text shows the correct next value
                    if( storedPadNumbers.size() > 0 )
                        newval = storedPadNumbers.front();
                    else
                        newval = seqPadNum;

                    statusPopup.SetText( wxString::Format( msg, padPrefix, newval ) );
                }

                // ... or restore the old name if it was enumerated and clicked again
                else if( pad->IsSelected() && evt->IsClick( BUT_LEFT ) )
                {
                    auto it = oldNames.find( pad->GetName() );
                    wxASSERT( it != oldNames.end() );

                    if( it != oldNames.end() )
                    {
                        storedPadNumbers.push_back( it->second.first );
                        pad->SetName( it->second.second );
                        oldNames.erase( it );

                        int newval = storedPadNumbers.front();

                        statusPopup.SetText( wxString::Format( msg, padPrefix, newval ) );
                    }

                    pad->ClearSelected();
                    getView()->Update( pad );
                }
            }
        }

        else if( ( evt->IsKeyPressed() && evt->KeyCode() == WXK_RETURN ) ||
                 evt->IsDblClick( BUT_LEFT ) )
        {
            commit.Push( _( "Renumber pads" ) );
            frame()->PopTool( tool );
            break;
        }

        else if( evt->IsClick( BUT_RIGHT ) )
        {
            m_menu.ShowContextMenu( selection() );
        }

        else
            evt->SetPassEvent();

        // Prepare the next loop by updating the old cursor mouse position
        // to this last mouse cursor position
        oldCursorPos = getViewControls()->GetCursorPosition();
        statusPopup.Move( wxGetMousePosition() + wxPoint( 20, 20 ) );
    }

    for( auto p : board()->GetFirstModule()->Pads() )
    {
        p->ClearSelected();
        view->Update( p );
    }

    statusPopup.Hide();
    return 0;
}


void PAD_TOOL::setTransitions()
{
    Go( &PAD_TOOL::pastePadProperties, PCB_ACTIONS::applyPadSettings.MakeEvent() );
    Go( &PAD_TOOL::copyPadSettings,    PCB_ACTIONS::copyPadSettings.MakeEvent() );
    Go( &PAD_TOOL::pushPadSettings,    PCB_ACTIONS::pushPadSettings.MakeEvent() );

    Go( &PAD_TOOL::EnumeratePads,      PCB_ACTIONS::enumeratePads.MakeEvent() );
}
