import layout from './layout'

const staticRoot = process.env.STATIC_ROOT || ''

// Concatena Labs explorer landing page. Reframed from Blockstream's hosted-API
// pitch into an open, self-hostable explorer for the Sequentia testnet and the
// Bitcoin testnet4 it anchors into. External links point at sequentia.io —
// update to Concatena's exact docs/site URLs when available.
const SITE = 'https://sequentia.io'
// TODO: point at Concatena's hosted API reference when available; the endpoints
// follow the Esplora REST API (see explorer/esplora/API.md in the repo).
const API_DOCS = 'https://sequentia.io'

const LandingPage = ({ t, ...S }) => layout(
    <div className="landing-page">
        <div className="blur-orange"></div>
        <div className="blur-green"></div>
        <div className="laser-lines"></div>
        <div className="hero-section">
        <div className="container">
            <div className="hero-wrapper">
                <div className="hero-text">
                    <h1 className="font-h1">Follow Sequentia<br/>down to Bitcoin</h1>
                    <p className="font-p1 text-gray">One explorer for the Sequentia testnet and the Bitcoin testnet4 it
anchors into.<br/> Blocks, transactions, assets, the proof-of-stake committee, and
every anchor —<br/> served over an open, fast REST API.</p>
                    <a href={API_DOCS} target="_blank" rel="noopener" className="g-btn primary-btn">READ THE API DOCS</a>
                </div>
                <div className="hero-image">
                    <img src={`${staticRoot}img/explorer-api-compass.png`} alt="Concatena explorer" />
                </div>
            </div>
        </div>
        </div>

        <div className="container">
        <div className="info-section">
            <div className="badge">WHY CONCATENA</div>
            <h2 className="font-h2">A sidechain explorer that doesn't stop at the sidechain</h2>
            <p className="font-p3 text-gray">Sequentia anchors every block into Bitcoin. This explorer lets you read both<br/> sides of that relationship in one place, with no keys and no quotas.</p>

            <div className="info-cards">
                <div className="info-card">
                    <img src={`${staticRoot}img/icons/integrate.svg`} alt="icon" />
                    <h3 className="font-h2">Sidechain and parent</h3>
                    <p className="font-p3 text-gray">Toggle between Sequentia and its Bitcoin testnet4 anchor in one click.</p>
                </div>
                <div className="info-card">
                    <img src={`${staticRoot}img/icons/redundancy.svg`} alt="icon" />
                    <h3 className="font-h2">Open and self-hostable</h3>
                    <p className="font-p3 text-gray">Run your own indexer. No API keys, no rate limits, no lock-in.</p>
                </div>
                <div className="info-card">
                    <img src={`${staticRoot}img/icons/privacy.svg`} alt="icon" />
                    <h3 className="font-h2">Built on Esplora</h3>
                    <p className="font-p3 text-gray">The battle-tested Esplora stack, extended for Sequentia's consensus.</p>
                </div>
            </div>
        </div>
        <div className="features-section">
            <div className="badge">WHAT YOU CAN SEE</div>
            <h2 className="font-h2">Every Sequentia-specific field, decoded and served alongside the standard explorer data</h2>
            <div className="features">
                <div className="feature">
                    <img src={`${staticRoot}img/icons/issuance.svg`} alt="icon" />
                    <p className="font-p3 text-gray">The Bitcoin testnet4 anchor<br/> committed in every block.</p>
                </div>
                <div className="feature">
                    <img src={`${staticRoot}img/icons/security-tokens.svg`} alt="icon" />
                    <p className="font-p3 text-gray">The proof-of-stake committee<br/> certificate: leader, members, BLS.</p>
                </div>
                <div className="feature">
                    <img src={`${staticRoot}img/icons/lbtc.svg`} alt="icon" />
                    <p className="font-p3 text-gray">Assets and issuances<br/> on the Sequentia chain.</p>
                </div>
                <div className="feature">
                    <img src={`${staticRoot}img/icons/encryption.svg`} alt="icon" />
                    <p className="font-p3 text-gray">Checkpoints and finality<br/> from the node.</p>
                </div>
                <div className="feature">
                    <img src={`${staticRoot}img/icons/integration.svg`} alt="icon" />
                    <p className="font-p3 text-gray">The full Bitcoin testnet4 chain<br/> the sidechain anchors to.</p>
                </div>
                <div className="feature">
                    <img src={`${staticRoot}img/icons/database.svg`} alt="icon" />
                    <p className="font-p3 text-gray">Pre-indexed data over REST<br/> for fast, scriptable access.</p>
                </div>
            </div>
        </div>
        </div>

        <div className="cta-section">
            <div className="landing-section-background"></div>
            <div className="container">
                <div className="cta-card">
                    <div className="cta-left">
                        <img src={`${staticRoot}img/icons/rest-api.svg`} alt="icon" />
                        <h2 className="font-h2">HTTP REST API<br/> for Sequentia and Bitcoin</h2>
                        <p className="font-p3 text-gray">Read blocks, transactions, UTXOs, mempool, and fees — plus the Bitcoin
anchor and the proof-of-stake committee certificate —<br/> through low-latency REST endpoints.</p>
                        <a href={API_DOCS} target="_blank" rel="noopener" className="g-btn primary-btn">EXPLORE DOCUMENTATION</a>
                    </div>
                    <div className="cta-right">
                        <img src={`${staticRoot}img/rest-api-cta.svg`} alt="HTTP REST API" />
                    </div>
                </div>
            </div>
        </div>

        <div className="cta-section">
            <div className="container">
                <div className="cta-card">
                    <div className="cta-left">
                        <h2 className="font-h2">Concatena Labs</h2>
                        <p className="font-p3 text-gray">Sequentia is a proof-of-stake, Bitcoin-anchored sidechain built by Concatena Labs.</p>
                        <a href={SITE} target="_blank" rel="noopener" className="g-btn primary-btn">VISIT SEQUENTIA.IO</a>
                    </div>
                </div>
            </div>
        </div>
    </div>
    , { t, activeTab: 'apiLanding', ...S })

export default LandingPage
