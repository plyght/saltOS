vim.g.mapleader = " "
vim.g.maplocalleader = " "

local o = vim.opt
o.number = true
o.relativenumber = true
o.signcolumn = "yes"
o.cursorline = true
o.termguicolors = true
o.expandtab = true
o.shiftwidth = 2
o.tabstop = 2
o.smartindent = true
o.ignorecase = true
o.smartcase = true
o.splitbelow = true
o.splitright = true
o.undofile = true
o.updatetime = 250
o.scrolloff = 6
o.clipboard = "unnamedplus"
o.mouse = "a"
o.wrap = false
o.showmode = false
o.completeopt = { "menuone", "noselect", "popup" }

local map = vim.keymap.set
map("n", "<leader>w", "<cmd>write<cr>", { desc = "Save" })
map("n", "<leader>q", "<cmd>quit<cr>", { desc = "Quit" })
map("n", "<Esc>", "<cmd>nohlsearch<cr>")
map("n", "<C-h>", "<C-w>h")
map("n", "<C-j>", "<C-w>j")
map("n", "<C-k>", "<C-w>k")
map("n", "<C-l>", "<C-w>l")
map("v", "J", ":m '>+1<cr>gv=gv")
map("v", "K", ":m '<-2<cr>gv=gv")

local lazypath = vim.fn.stdpath("data") .. "/lazy/lazy.nvim"
if not vim.uv.fs_stat(lazypath) then
  vim.fn.system({
    "git", "clone", "--filter=blob:none", "--branch=stable",
    "https://github.com/folke/lazy.nvim.git", lazypath,
  })
end
vim.opt.rtp:prepend(lazypath)

local ok, theme = pcall(require, "saltos_theme")
if not ok then
  theme = { plugin = "folke/tokyonight.nvim", colorscheme = "tokyonight-night" }
end

require("lazy").setup({
  spec = {
    {
      theme.plugin,
      lazy = false,
      priority = 1000,
      config = function()
        vim.cmd.colorscheme(theme.colorscheme)
      end,
    },
    { import = "plugins" },
  },
  install = { colorscheme = { theme.colorscheme } },
  checker = { enabled = false },
  change_detection = { notify = false },
})

vim.api.nvim_create_autocmd("Signal", {
  pattern = "SIGUSR1",
  callback = function()
    package.loaded["saltos_theme"] = nil
    local fresh = require("saltos_theme")
    pcall(vim.cmd.colorscheme, fresh.colorscheme)
  end,
})
