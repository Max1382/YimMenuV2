#include "Clothing.hpp"
#include "Self/OutfitEditor.hpp"
#include "Self/SavedOutfitNames.hpp"
#include "game/frontend/items/Items.hpp"

namespace YimMenu::Submenus
{
	Clothing::Clothing() : Submenu("Clothing", "")
	{
		AddCategory(CreateOutfitsMenu());
		auto saved = std::make_shared<Category>("Saved Outfits");
		saved->AddItem(std::make_shared<ImGuiItem>([] { DrawMax1382SavedOutfits(); }));
		AddCategory(std::move(saved));
	}
}
