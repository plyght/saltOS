return {
  {
    "nvim-treesitter/nvim-treesitter",
    build = ":TSUpdate",
    event = { "BufReadPost", "BufNewFile" },
    main = "nvim-treesitter.configs",
    opts = {
      ensure_installed = { "bash", "c", "cpp", "lua", "markdown", "markdown_inline", "toml", "json", "yaml", "python", "rust", "go" },
      auto_install = true,
      highlight = { enable = true },
      indent = { enable = true },
    },
  },
  {
    "nvim-telescope/telescope.nvim",
    dependencies = { "nvim-lua/plenary.nvim" },
    cmd = "Telescope",
    keys = {
      { "<leader>f", "<cmd>Telescope find_files<cr>", desc = "Find files" },
      { "<leader>g", "<cmd>Telescope live_grep<cr>", desc = "Grep" },
      { "<leader>b", "<cmd>Telescope buffers<cr>", desc = "Buffers" },
      { "<leader>h", "<cmd>Telescope help_tags<cr>", desc = "Help" },
      { "<leader>d", "<cmd>Telescope diagnostics<cr>", desc = "Diagnostics" },
    },
    opts = { defaults = { layout_strategy = "flex", sorting_strategy = "ascending", layout_config = { prompt_position = "top" } } },
  },
  {
    "neovim/nvim-lspconfig",
    event = { "BufReadPre", "BufNewFile" },
    config = function()
      local servers = { "clangd", "lua_ls", "bashls", "pyright", "rust_analyzer", "gopls", "ts_ls" }
      for _, server in ipairs(servers) do
        vim.lsp.enable(server)
      end
      vim.api.nvim_create_autocmd("LspAttach", {
        callback = function(ev)
          local client = vim.lsp.get_client_by_id(ev.data.client_id)
          if client and client:supports_method("textDocument/completion") then
            vim.lsp.completion.enable(true, client.id, ev.buf, { autotrigger = true })
          end
          local b = { buffer = ev.buf }
          vim.keymap.set("n", "gd", vim.lsp.buf.definition, b)
          vim.keymap.set("n", "gr", vim.lsp.buf.references, b)
          vim.keymap.set("n", "K", vim.lsp.buf.hover, b)
          vim.keymap.set("n", "<leader>r", vim.lsp.buf.rename, b)
          vim.keymap.set("n", "<leader>a", vim.lsp.buf.code_action, b)
          vim.keymap.set("n", "<leader>=", function() vim.lsp.buf.format({ async = true }) end, b)
        end,
      })
      vim.diagnostic.config({ virtual_text = true, severity_sort = true })
    end,
  },
  {
    "lewis6991/gitsigns.nvim",
    event = { "BufReadPre", "BufNewFile" },
    opts = {},
  },
  {
    "echasnovski/mini.nvim",
    event = "VeryLazy",
    config = function()
      require("mini.pairs").setup()
      require("mini.surround").setup()
      require("mini.comment").setup()
      require("mini.statusline").setup()
      require("mini.icons").setup()
    end,
  },
}
